#include "hv/requests.h"
#include "hv/hurl.h"
#include "config.h"
#include "state.h"
#include "logger.h"
#include "platform.h"

#include <cmath>
#include <time.h>
#include <sstream>
#include <iomanip>
#include <experimental/filesystem>
#include <regex>

namespace fs = std::experimental::filesystem;

/* The [moonraker] host placeholder, and the address it stands for. */
#define KUTIL_HOST_ANY      "0.0.0.0"    // host placeholder: the Moonraker server runs on this machine
#define KUTIL_HOST_LOOPBACK "127.0.0.1"  // what the placeholder is replaced with: this machine's loopback

namespace KUtils {
  std::string get_root_path(const std::string root_name) {
    auto roots = State::get_instance()->get_data("/roots"_json_pointer);
    json filtered;
    std::copy_if(roots.begin(), roots.end(),
		 std::back_inserter(filtered), [&root_name](const json& item) {
		   return item.contains("name") && item["name"] == root_name;
		 });

    if (get_log_level() <= LogLevel::TRACE) {
      LOG_TRACE("roots {}, filtered {}", roots.dump(), filtered.dump());
    }
    if (!filtered.empty()) {
      return filtered["/0/path"_json_pointer];
    }

    return "";
  }

  /*------------------------------------------------------------------------*/
  /* KUTIL_MoonrakerHost:                                                   */
  /*                                                                        */
  /* The address GrumpyScreen reaches Moonraker at: [moonraker] host as     */
  /* configured, except that 0.0.0.0 -- the "any address" placeholder -- is */
  /* read as THIS machine and becomes 127.0.0.1.  So a configuration can    */
  /* ship with 0.0.0.0 and work as it stands wherever the Moonraker server  */
  /* runs on the same machine as GrumpyScreen.  An address set by hand is   */
  /* used exactly as written.                                               */
  /*                                                                        */
  /* Every reader of the host goes through here, not the configuration,    */
  /* because the readers must agree: the websocket connection and          */
  /* get_thumbnail's "is Moonraker on this machine?" test.  If only the     */
  /* connection mapped the placeholder, get_thumbnail would still see       */
  /* 0.0.0.0, count Moonraker as remote and -- with no thumbnail_path set   */
  /* -- show no thumbnails at all.                                          */
  /*                                                                        */
  /* Arguments:                                                             */
  /*     None.                                                              */
  /*                                                                        */
  /* Returns:                                                               */
  /*     std::string : the host to connect to -- the configured one, or     */
  /*                   127.0.0.1 in place of 0.0.0.0; empty when none is    */
  /*                   configured.                                          */
  /*------------------------------------------------------------------------*/

  std::string KUTIL_MoonrakerHost(void)
  {
      std::string sHost;

      /* The host as the configuration (and any override file) gives it. */
      sHost = Config::get_instance()->get<std::string>("/moonraker/host");

      if (sHost == KUTIL_HOST_ANY)
      {   /* The any-address placeholder: Moonraker is on this machine. */
          sHost = KUTIL_HOST_LOOPBACK;
      }

      /* The host to connect to. */
      return(sHost);
  }

  /*------------------------------------------------------------------------*/
  /* get_thumbnail:                                                         */
  /*                                                                        */
  /* Find a displayable image for a gcode file's thumbnail.  Moonraker's    */
  /* file metadata lists the thumbnails the slicer embedded, each with a    */
  /* width and a path relative to the gcode file's own directory.  Taking   */
  /* them to be square, this picks the one whose width is nearest the       */
  /* caller's target, then returns where that image can be read:            */
  /*   - Moonraker on this machine (the host, after KUTIL_MoonrakerHost, is */
  /*     127.0.0.1 or localhost) and no [moonraker] thumbnail_path: the     */
  /*     file where it lies, under Moonraker's gcodes root, read from disk; */
  /*   - otherwise: a copy downloaded over HTTP from Moonraker into the     */
  /*     thumbnail_path directory, under the image's bare file name, which  */
  /*     needs thumbnail_path set and existing.                             */
  /* A thumbnail_path that is set therefore forces the download even when   */
  /* Moonraker is on this machine.                                          */
  /*                                                                        */
  /* Arguments:                                                             */
  /*     gcode_file   : the gcode file's path within Moonraker's gcodes     */
  /*                    root, the base the thumbnail's path hangs off.      */
  /*     j            : Moonraker's metadata reply for that file; its       */
  /*                    /result/thumbnails list is read, and a reply with   */
  /*                    none is left holding a null there, which counts as  */
  /*                    no thumbnails.                                      */
  /*     target_width : the width, in pixels, the caller will show it at.   */
  /*                                                                        */
  /* Returns:                                                               */
  /*     std::pair<std::string, size_t> : the image's local path and the    */
  /*                    chosen thumbnail's width; ("", 0) when there are no */
  /*                    thumbnails or no path can be made.  The width is    */
  /*                    filled in only when a later thumbnail beats the     */
  /*                    first, so it is 0 when the first is the nearest;    */
  /*                    both callers read only the path.                    */
  /*------------------------------------------------------------------------*/

  std::pair<std::string, size_t> get_thumbnail(const std::string &gcode_file, json &j, int target_width) {
    /* The thumbnails Moonraker's metadata lists for this file. */
    auto &thumbs = j["/result/thumbnails"_json_pointer];
    if (!thumbs.is_null() && !thumbs.empty()) {  /* At least one: choose it, then find its image. */
      /* Width alone decides, the thumbnails taken to be square: start    */
      /* from the first, then keep whichever is nearest the target.  A    */
      /* width may arrive as a number or as a string, so both are read.   */
      const int scaled_width = target_width;
      LOG_DEBUG("using thumb at scaled width {}", scaled_width);
      uint32_t closest_index = 0;
      size_t thumb_width = 0;
      auto width = thumbs.at(0)["width"].is_number()
	        ? thumbs.at(0)["width"].template get<int>()
	        : std::stoi(thumbs.at(0)["width"].template get<std::string>());
      int closest = std::abs(scaled_width - width);
      for (int i = 0; i < thumbs.size(); i++) {  /* One listed thumbnail per iteration. */
        /* Its width, a number either way, and how far it is from the target. */
	      width = thumbs.at(i)["width"].is_number()
	        ? thumbs.at(i)["width"].template get<int>()
	        : std::stoi(thumbs.at(i)["width"].template get<std::string>());
	      int cur_diff = std::abs(scaled_width - width);
        if (cur_diff < closest) {  /* Nearer the target than any before: it becomes the choice. */
          closest = cur_diff;
          closest_index = i;
          thumb_width = width;
        }
      }

      /* The chosen thumbnail; at debug level the whole list is logged too. */
      auto &thumb = thumbs.at(closest_index);
      if (get_log_level() <= LogLevel::DEBUG) {  /* Debug logging: which one was taken, from what. */
        LOG_DEBUG("using thumb at index {}, {}", closest_index, thumbs.dump());
      }

      /* Its path.  Metadata gives it relative to the gcode file's own      */
      /* directory, so that directory is put in front; a gcode file at the  */
      /* root has none, and the path stands as given.                       */
      std::string relative_path = thumb["relative_path"].template get<std::string>();
      size_t found = gcode_file.find_last_of("/\\");
      if (found != std::string::npos) {  /* The gcode file is in a sub-directory: prefix it. */
	      relative_path = gcode_file.substr(0, found + 1) + relative_path;
      }

      /* Where Moonraker is -- through KUTIL_MoonrakerHost, so a configured */
      /* 0.0.0.0 arrives here as 127.0.0.1 -- and the image's bare name.     */
      Config *conf = Config::get_instance();
      std::string moonraker_host = KUTIL_MoonrakerHost();
      std::string fname = relative_path.substr(relative_path.find_last_of("/\\") + 1);

      /* Moonraker counts as on this machine at 127.0.0.1 or localhost;    */
      /* the download directory is empty when none is configured.          */
      const bool is_running_local = moonraker_host == "localhost" || moonraker_host == "127.0.0.1";
      std::string thumbnail_path = conf->get<std::string>("/moonraker/thumbnail_path");

      if (!is_running_local && thumbnail_path == "") {  /* Moonraker elsewhere and nowhere to download to: no thumbnail. */
        LOG_ERROR("Thumbnail path is not defined");
        return std::make_pair("", 0);
      }

      std::string fullpath;
      if (is_running_local && thumbnail_path == "") {  /* Moonraker on this machine: the file where it lies, under the gcodes root. */
        auto gcode_root = get_root_path("gcodes");
        fullpath = fmt::format("{}/{}", gcode_root, relative_path);
      } else {  /* A download directory is set (always, by here, when Moonraker is elsewhere): fetch the image into it. */
        if (fs::exists(fs::path(thumbnail_path))) {  /* The directory exists: download into it under the bare name. */
          /* Where it lands, and Moonraker's file route for it: the path is */
          /* escaped for the URL, its slashes kept.                        */
          fullpath = fmt::format("{}/{}", thumbnail_path, fname);
          std::string thumb_url = fmt::format("http://{}:{}/server/files/gcodes/{}",
                      moonraker_host,
                      conf->get<uint32_t>("/moonraker/port"),
                      HUrl::escape(relative_path, "/"));

          /* Fetch it.  The size is only logged: the path is returned below */
          /* whether or not the download succeeded.                         */
          LOG_DEBUG("Download thumb {} -> {}", thumb_url, fullpath);
          auto size = requests::downloadFile(thumb_url.c_str(), fullpath.c_str());
          LOG_TRACE("downloaded size {}", size);
        } else {  /* The configured directory is missing: no thumbnail. */
          LOG_ERROR("Thumbnail path {} does not exist", thumbnail_path);
          return std::make_pair("", 0);
        }
      }

      /* The image's path, and the chosen width (0 if the first was nearest). */
      return std::make_pair(fullpath, thumb_width);
    }

    /* No thumbnails in the metadata. */
    return std::make_pair("", 0);
  }

  std::string get_obj_name(const std::string &id) {
    size_t pos = id.find_last_of(' ');
    return id.substr(pos + 1);
  }

  std::string to_title(std::string s) {
    bool last = true;
    for (char& c : s) {
      c = last ? std::toupper(c) : std::tolower(c);
      if (c == '_') {
	      c = ' ';
      }
      last = std::isspace(c);
    }
    return s;
  }

  std::string eta_string(int64_t s) {
    time_t seconds (s);
    tm p;
    gmtime_r (&seconds, &p);

    std::ostringstream os;

    if (p.tm_yday > 0)
      os << p.tm_yday << "d ";

    if (p.tm_hour > 0)
      os << p.tm_hour << "h ";

    if (p.tm_min > 0)
      os << p.tm_min << "m ";

    os << p.tm_sec << "s";
    
    return os.str();
  }

  size_t bytes_to_mb(size_t s) {
    return s / 1024 / 1024;
  }
}  // namespace KUtils
