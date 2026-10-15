/**
 * @file luminousbugreport.cpp
 * @brief Report a Bug: a report sent to Needful, with the viewer's logs and a picture.
 *
 * $LicenseInfo:firstyear=2026&license=fsviewerlgpl$
 * Luminous Viewer Source Code
 * Copyright (C) 2026, Catten Carter
 * Based on the Phoenix Firestorm Viewer, Copyright (C) 2026,
 * The Phoenix Firestorm Project, Inc.
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation;
 * version 2.1 of the License only.
 *
 * This library is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * Lesser General Public License for more details.
 *
 * You should have received a copy of the GNU Lesser General Public
 * License along with this library; if not, write to the Free Software
 * Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston, MA  02110-1301  USA
 *
 * http://www.firestormviewer.org
 * $/LicenseInfo$
 */

#include "llviewerprecompiledheaders.h"

#include "luminousbugreport.h"
#include "luminousbugreportkey.h"

#include "indra_constants.h"
#include "llagent.h"
#include "llagentdata.h"
#include "llagentui.h"
#include "llappviewer.h"
#include "llbutton.h"
#include "llcheckboxctrl.h"
#include "llcombobox.h"
#include "llcorehttputil.h"
#include "llcoros.h"
#include "lldir.h"
#include "llerrorcontrol.h"
#include "lleventcoro.h"
#include "llfile.h"
#include "llfilepicker.h"
#include "llhttpconstants.h"
#include "llimage.h"
#include "llimagejpeg.h"
#include "llnotificationsutil.h"
#include "lllineeditor.h"
#include "llsdjson.h"
#include "llsdutil.h"
#include "llsys.h"
#include "lltextbox.h"
#include "lltexteditor.h"
#include "llversioninfo.h"
#include "llviewercontrol.h"
#include "llviewermenufile.h"
#include "llviewernetwork.h"
#include "llviewerregion.h"

#include "zlib.h"

namespace
{
    // The API's limits (GET on the endpoint gives them live): 16 MB a file
    // as sent, 8 MB and 12000 pixels a side for a picture. A log is read
    // from its end -- where a crash is -- and no more of it than the server
    // keeps, the last 20 MB of its text; that gzips well under 16 MB.
    const size_t MAX_LOG_TEXT = 20 * 1024 * 1024;
    const size_t MAX_PICTURE_BYTES = 8 * 1024 * 1024;
    const S32 MAX_PICTURE_SIDE = 12000;

    bool hasKey()
    {
        return LUMINOUS_BUG_REPORT_KEY[0] != '\0';
    }

    // The last session did not end cleanly: crashed, froze, or was killed.
    bool lastSessionCrashed()
    {
        switch (gLastExecEvent)
        {
            case LAST_EXEC_FROZE:
            case LAST_EXEC_LLERROR_CRASH:
            case LAST_EXEC_OTHER_CRASH:
            case LAST_EXEC_LOGOUT_FROZE:
            case LAST_EXEC_LOGOUT_CRASH:
            case LAST_EXEC_BAD_ALLOC:
            case LAST_EXEC_UNKNOWN:
            case LAST_EXEC_LOGOUT_UNKNOWN:
                return true;
            default:
                return false;
        }
    }

    std::string currentLogPath()
    {
        std::string path = LLError::logFileName();
        return path.empty() ? gDirUtilp->getExpandedFilename(LL_PATH_LOGS, APP_NAME + ".log") : path;
    }

    std::string previousLogPath()
    {
        return gDirUtilp->getExpandedFilename(LL_PATH_LOGS, APP_NAME + ".old");
    }

    // The end of a file, at most max bytes of it.
    bool readTail(const std::string& path, size_t max, std::string& out)
    {
        out.clear();
        llifstream in(path.c_str(), std::ios::in | std::ios::binary);
        if (!in.is_open())
        {
            return false;
        }
        in.seekg(0, std::ios::end);
        const std::streamoff size = in.tellg();
        if (size <= 0)
        {
            return false;
        }
        const std::streamoff start = size > (std::streamoff)max ? size - (std::streamoff)max : 0;
        in.seekg(start, std::ios::beg);
        out.resize((size_t)(size - start));
        in.read(&out[0], (std::streamsize)out.size());
        out.resize((size_t)in.gcount());
        return !out.empty();
    }

    // gzip in memory, as the API asks: a viewer log shrinks ten to twenty
    // times. Window bits 15 + 16 write the gzip header the server looks for.
    bool gzipString(const std::string& in, std::string& out)
    {
        z_stream strm = {};
        if (deflateInit2(&strm, Z_DEFAULT_COMPRESSION, Z_DEFLATED, 15 + 16, 8, Z_DEFAULT_STRATEGY) != Z_OK)
        {
            return false;
        }
        out.resize(deflateBound(&strm, (uLong)in.size()));
        strm.next_in = (Bytef*)in.data();
        strm.avail_in = (uInt)in.size();
        strm.next_out = (Bytef*)&out[0];
        strm.avail_out = (uInt)out.size();
        const int ret = deflate(&strm, Z_FINISH);
        const size_t written = out.size() - strm.avail_out;
        deflateEnd(&strm);
        if (ret != Z_STREAM_END)
        {
            out.clear();
            return false;
        }
        out.resize(written);
        return true;
    }

    // A picture as the API takes it: PNG or JPEG of at most 8 MB and 12000
    // pixels a side. A PNG or JPEG within that goes as it is; anything else
    // -- a render at twice a 5K window is a 12 MB PNG, or a BMP or TGA --
    // as a JPEG at quality 92, made smaller until it fits.
    bool preparePicture(const std::string& path, std::string& data, std::string& mime, std::string& filename,
                        std::string& error)
    {
        std::string ext = gDirUtilp->getExtension(path);
        LLStringUtil::toLower(ext);
        if ((ext == "png" || ext == "jpg" || ext == "jpeg") && readTail(path, MAX_PICTURE_BYTES + 1, data)
            && data.size() <= MAX_PICTURE_BYTES)
        {
            mime = ext == "png" ? "image/png" : "image/jpeg";
            filename = ext == "png" ? "picture.png" : "picture.jpg";
            return true;
        }
        data.clear();
        LLPointer<LLImageFormatted> image = LLImageFormatted::createFromExtension(path);
        LLPointer<LLImageRaw> raw = new LLImageRaw;
        if (image.isNull() || !image->load(path) || !image->decode(raw, 0.f) || raw->getWidth() <= 0)
        {
            error = "The picture could not be read.";
            return false;
        }
        if (raw->getComponents() != 3 && raw->getComponents() != 1)
        {
            LLPointer<LLImageRaw> rgb = new LLImageRaw(raw->getWidth(), raw->getHeight(), 3);
            rgb->copy(raw);
            raw = rgb;
        }
        const S32 side = llmax(raw->getWidth(), raw->getHeight());
        if (side > MAX_PICTURE_SIDE)
        {
            const F32 scale = (F32)MAX_PICTURE_SIDE / (F32)side;
            raw->scale(llmax(1, (S32)(raw->getWidth() * scale)), llmax(1, (S32)(raw->getHeight() * scale)));
        }
        for (S32 attempt = 0; attempt < 8; ++attempt)
        {
            LLPointer<LLImageJPEG> jpeg = new LLImageJPEG(92);
            if (!jpeg->encode(raw, 0.f))
            {
                error = "The picture could not be prepared.";
                return false;
            }
            if ((size_t)jpeg->getDataSize() <= MAX_PICTURE_BYTES)
            {
                data.assign((const char*)jpeg->getData(), (size_t)jpeg->getDataSize());
                mime = "image/jpeg";
                filename = "picture.jpg";
                return true;
            }
            raw->scale(llmax(1, raw->getWidth() * 3 / 4), llmax(1, raw->getHeight() * 3 / 4));
        }
        error = "The picture is too large to send.";
        return false;
    }

    struct Report
    {
        std::vector<std::pair<std::string, std::string>> fields;
        bool log = true;
        bool previous_log = false;
        std::string picture;
    };

    class Multipart
    {
    public:
        Multipart()
            : mBody(new LLCore::BufferArray)
            , mBoundary("----LuminousReport" + LLUUID::generateNewID().asString())
        {
        }

        void field(const std::string& name, const std::string& value)
        {
            add("--" + mBoundary + "\r\nContent-Disposition: form-data; name=\"" + name + "\"\r\n\r\n");
            add(value);
            add("\r\n");
        }

        void file(const std::string& name, const std::string& filename, const std::string& mime, const std::string& data)
        {
            add("--" + mBoundary + "\r\nContent-Disposition: form-data; name=\"" + name + "\"; filename=\"" + filename
                + "\"\r\nContent-Type: " + mime + "\r\n\r\n");
            add(data);
            add("\r\n");
        }

        LLCore::BufferArray::ptr_t finish()
        {
            add("--" + mBoundary + "--\r\n");
            return mBody;
        }

        std::string contentType() const { return "multipart/form-data; boundary=" + mBoundary; }
        size_t size() const { return mBody->size(); }

    private:
        void add(const std::string& s)
        {
            if (!s.empty())
            {
                mBody->append(s.data(), s.size());
            }
        }

        LLCore::BufferArray::ptr_t mBody;
        std::string mBoundary;
    };

    // What the user is told for each way the server can say no (the API's
    // own text on success).
    std::string replyText(S32 code, const LLSD& reply, const LLCore::HttpStatus& status)
    {
        const std::string error = reply["error"].asString();
        if (code == 400 && error == "invalid" && reply["fields"].isMap())
        {
            std::string text = "Please look at:";
            for (const auto& field : llsd::inMap(reply["fields"]))
            {
                text += " " + field.second.asString();
            }
            return text;
        }
        switch (code)
        {
            case 400: return "The server could not read the picture or a log. Try again without the picture.";
            case 404: return "This version of Luminous can no longer send reports. Please update it.";
            case 413: return "The report is too large. Try again without the picture or the last session's log.";
            case 429: return "Too many reports have been sent from here in the last hour. Please try again later.";
            case 503: return "The report server cannot take reports just now. Your text is kept: please try again later.";
            default: break;
        }
        if (code > 0)
        {
            return llformat("The report server answered %d. Please try again later.", code);
        }
        return "Could not reach the report server (" + status.toString() + "). Please try again later.";
    }

    void sendCoro(LLHandle<LuminousBugReportFloater> handle, Report report)
    {
        // Let "Sending..." show before the logs are read and packed.
        llcoro::suspend();

        Multipart body;
        for (const auto& field : report.fields)
        {
            body.field(field.first, field.second);
        }
        std::string text, packed;
        if (report.log && readTail(currentLogPath(), MAX_LOG_TEXT, text) && gzipString(text, packed))
        {
            body.file("log", "luminous.log.gz", "application/gzip", packed);
        }
        if (report.previous_log && readTail(previousLogPath(), MAX_LOG_TEXT, text) && gzipString(text, packed))
        {
            body.file("log_previous", "luminous-previous.log.gz", "application/gzip", packed);
        }
        text.clear();
        packed.clear();
        if (!report.picture.empty())
        {
            std::string data, mime, filename, error;
            if (!preparePicture(report.picture, data, mime, filename, error))
            {
                if (LuminousBugReportFloater* floater = handle.get())
                {
                    floater->sent(false, error);
                }
                return;
            }
            body.file("screenshot", filename, mime, data);
        }
        const std::string content_type = body.contentType();
        LLCore::BufferArray::ptr_t raw = body.finish();
        LL_INFOS("LuminousBugReport") << "Sending a bug report, " << raw->size() << " bytes" << LL_ENDL;

        LLCoreHttpUtil::HttpCoroutineAdapter::ptr_t adapter =
            std::make_shared<LLCoreHttpUtil::HttpCoroutineAdapter>("LuminousBugReport", LLCore::HttpRequest::DEFAULT_POLICY_ID);
        LLCore::HttpRequest::ptr_t request = std::make_shared<LLCore::HttpRequest>();
        LLCore::HttpOptions::ptr_t options = std::make_shared<LLCore::HttpOptions>();
        options->setRetries(0); // a report sent twice is two reports
        options->setTimeout(180);
        options->setTransferTimeout(180);
        LLCore::HttpHeaders::ptr_t headers = std::make_shared<LLCore::HttpHeaders>();
        headers->append(HTTP_OUT_HEADER_CONTENT_TYPE, content_type);
        headers->append(HTTP_OUT_HEADER_ACCEPT, "application/json");
        headers->append("X-Luminous-Key", LUMINOUS_BUG_REPORT_KEY);

        const std::string url = gSavedSettings.getString("LuminousBugReportURL");
        LLSD result = adapter->postRawAndSuspend(request, url, raw, options, headers);

        const LLCore::HttpStatus status =
            LLCoreHttpUtil::HttpCoroutineAdapter::getStatusFromLLSD(result[LLCoreHttpUtil::HttpCoroutineAdapter::HTTP_RESULTS]);
        const S32 code = status.isHttpStatus() ? (S32)status.getType() : 0;
        const LLSD::Binary& bytes = result[LLCoreHttpUtil::HttpCoroutineAdapter::HTTP_RESULTS_RAW].asBinary();
        const std::string answer(bytes.begin(), bytes.end());
        LLSD reply;
        if (!answer.empty())
        {
            boost::system::error_code ec;
            boost::json::value json = boost::json::parse(answer, ec);
            if (!ec)
            {
                reply = LlsdFromJson(json);
            }
        }
        const bool ok = (code == 201 || code == 200) && reply["ok"].asBoolean();
        LL_INFOS("LuminousBugReport") << "Bug report answered " << code << (ok ? ": saved as #" + reply["id"].asString() : "")
                                      << LL_ENDL;
        if (LuminousBugReportFloater* floater = handle.get())
        {
            std::string message = reply["message"].asString();
            if (ok && message.empty())
            {
                message = "Thank you: report #" + reply["id"].asString() + " received.";
            }
            floater->sent(ok, ok ? message : replyText(code, reply, status));
        }
    }
}

LuminousBugReportFloater::LuminousBugReportFloater(const LLSD& key)
    : LLFloater(key)
{
}

bool LuminousBugReportFloater::postBuild()
{
    getChild<LLButton>("send")->setCommitCallback([this](LLUICtrl*, const LLSD&) { onSend(); });
    getChild<LLButton>("cancel")->setCommitCallback([this](LLUICtrl*, const LLSD&) { closeFloater(); });
    getChild<LLButton>("choose_picture")->setCommitCallback([this](LLUICtrl*, const LLSD&) { onChoosePicture(); });
    getChild<LLCheckBoxCtrl>("include_picture")->setCommitCallback(
        [this](LLUICtrl* ctrl, const LLSD&)
        {
            if (ctrl->getValue().asBoolean() && mPicturePath.empty())
            {
                ctrl->setValue(false); // ticked once a picture is chosen
                onChoosePicture();
            }
        });
    getChild<LLCheckBoxCtrl>("include_log")->set(true);
    return true;
}

void LuminousBugReportFloater::onOpen(const LLSD& key)
{
    if (mSending)
    {
        return;
    }
    // After a crash, the log that tells of it is the last session's.
    const bool crashed = lastSessionCrashed();
    LLCheckBoxCtrl* previous = getChild<LLCheckBoxCtrl>("include_previous_log");
    previous->setEnabled(LLFile::isfile(previousLogPath()));
    previous->set(crashed && previous->getEnabled());
    LLComboBox* category = getChild<LLComboBox>("category");
    if (crashed && category->getValue().asString() == "other")
    {
        category->setValue("crash");
    }
    if (!hasKey())
    {
        showStatus("This build of Luminous cannot send reports.");
        getChild<LLButton>("send")->setEnabled(false);
    }
}

void LuminousBugReportFloater::onChoosePicture()
{
    LLHandle<LuminousBugReportFloater> handle = getDerivedHandle<LuminousBugReportFloater>();
    LLFilePickerReplyThread::startPicker(
        [handle](const std::vector<std::string>& files, LLFilePicker::ELoadFilter, LLFilePicker::ESaveFilter)
        {
            LuminousBugReportFloater* floater = handle.get();
            if (!floater || files.empty())
            {
                return;
            }
            floater->mPicturePath = files[0];
            floater->getChild<LLTextBox>("picture_name")->setText(gDirUtilp->getBaseFileName(files[0]));
            floater->getChild<LLCheckBoxCtrl>("include_picture")->set(true);
        },
        LLFilePicker::FFLOAD_IMAGE, false);
}

void LuminousBugReportFloater::showStatus(const std::string& text)
{
    getChild<LLTextBox>("status")->setText(text);
}

void LuminousBugReportFloater::onSend()
{
    if (mSending || !hasKey())
    {
        return;
    }
    std::string summary = getChild<LLLineEditor>("summary")->getText();
    std::string details = getChild<LLTextEditor>("details")->getText();
    std::string steps = getChild<LLTextEditor>("steps")->getText();
    std::string email = getChild<LLLineEditor>("email")->getText();
    LLStringUtil::trim(summary);
    LLStringUtil::trim(details);
    LLStringUtil::trim(steps);
    LLStringUtil::trim(email);
    if (utf8str_to_wstring(summary).size() < 4)
    {
        showStatus("Please write a summary: a line about what went wrong.");
        return;
    }
    if (utf8str_to_wstring(details).size() < 3)
    {
        showStatus("Please tell what happened.");
        return;
    }

    Report report;
    auto add = [&report](const std::string& name, const std::string& value, S32 max_bytes)
    {
        if (!value.empty())
        {
            report.fields.emplace_back(name, utf8str_truncate(value, max_bytes));
        }
    };
    add("summary", summary, 150);
    add("details", details, 5000);
    add("category", getChild<LLComboBox>("category")->getValue().asString(), 20);
    add("steps", steps, 3000);
    add("email", email, 254);
    add("version", LLVersionInfo::instance().getVersion() + " (Firestorm " + LLVersionInfo::instance().getFirestormVersion() + ")", 60);
    add("channel", LLVersionInfo::instance().getChannel(), 60);
    add("macos", LLOSInfo::instance().getOSStringSimple(), 60);
#if defined(__arm64__) || defined(__aarch64__)
    add("mac", "arm64", 20);
#else
    add("mac", "x86_64", 20);
#endif
    add("system", LLAppViewer::instance()->getViewerInfoString(true), 8000);
    std::string name;
    LLAgentUI::buildFullname(name);
    add("avatar", name, 63);
    if (gAgentID.notNull())
    {
        add("agent_id", gAgentID.asString(), 36);
    }
    add("grid", LLGridManager::getInstance()->getGridId(), 60);
    if (LLViewerRegion* region = gAgent.getRegion())
    {
        add("region", region->getName(), 100);
    }
    report.log = getChild<LLCheckBoxCtrl>("include_log")->get();
    report.previous_log = getChild<LLCheckBoxCtrl>("include_previous_log")->get();
    if (getChild<LLCheckBoxCtrl>("include_picture")->get())
    {
        report.picture = mPicturePath;
    }

    mSending = true;
    getChild<LLButton>("send")->setEnabled(false);
    showStatus("Sending...");
    LLHandle<LuminousBugReportFloater> handle = getDerivedHandle<LuminousBugReportFloater>();
    LLCoros::instance().launch("LuminousBugReport", [handle, report]() { sendCoro(handle, report); });
}

void LuminousBugReportFloater::sent(bool success, const std::string& message)
{
    mSending = false;
    getChild<LLButton>("send")->setEnabled(hasKey());
    if (!success)
    {
        showStatus(message); // the window stays, the text with it
        return;
    }
    // Received: the window closes, its text cleared for the next report, and
    // the thanks show as a tip.
    showStatus(LLStringUtil::null);
    getChild<LLLineEditor>("summary")->clear();
    getChild<LLTextEditor>("details")->clear();
    getChild<LLTextEditor>("steps")->clear();
    getChild<LLCheckBoxCtrl>("include_picture")->set(false);
    getChild<LLTextBox>("picture_name")->setText(std::string("No picture chosen"));
    mPicturePath.clear();
    closeFloater();
    LLNotificationsUtil::add("SystemMessageTip", LLSD().with("MESSAGE", message));
}
