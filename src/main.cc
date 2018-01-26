// This file is part of the brlaser printer driver.
//
// Copyright 2013 Peter De Wachter
//
// brlaser is free software: you can redistribute it and/or modify
// it under the terms of the GNU General Public License as published by
// the Free Software Foundation, either version 2 of the License, or
// (at your option) any later version.
//
// brlaser is distributed in the hope that it will be useful,
// but WITHOUT ANY WARRANTY; without even the implied warranty of
// MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
// GNU General Public License for more details.
//
// You should have received a copy of the GNU General Public License
// along with brlaser.  If not, see <http://www.gnu.org/licenses/>.

#include <stdio.h>
#include <signal.h>
#include <locale.h>
#include <unistd.h>
#include <fcntl.h>
#include <cups/raster.h>
#include <algorithm>
#include <functional>
#include <string>
#include <array>
#include <map>
#include "config.h"
#include "job.h"
#include "debug.h"

namespace {

volatile sig_atomic_t interrupted = 0;

void sigterm_handler(int sig) {
  interrupted = 1;
}


cups_raster_t *ras;
cups_page_header2_t header;

bool next_line(uint8_t *buf) {
  if (interrupted) {
    return false;
  }
  unsigned bytes = header.cupsBytesPerLine;
  return cupsRasterReadPixels(ras, buf, bytes) == bytes;
}

bool plain_ascii_string(const char *str) {
  bool result = true;
  for (; result && *str; str++) {
    result = *str >= 32 && *str <= 126;
  }
  return result;
}

std::string ascii_job_name(const char *job_id, const char *job_user, const char *job_name) {
  std::array<const char *, 3> parts = {{
    job_id,
    job_user,
    job_name
  }};
  std::string result;
  for (const char *part : parts) {
    if (*part && plain_ascii_string(part)) {
      if (!result.empty()) {
        result += '/';
      }
      result += part;
    }
  }
  if (result.empty()) {
    result = "brlaser";
  }
  const int max_size = 79;
  if (result.size() > max_size) {
    result.resize(max_size);
  }
  return result;
}

page_params build_page_params() {
  static const std::array<std::string, 6> sources = {{
    "AUTO", "T1", "T2", "T3", "MP", "MANUAL"
  }};
  static const std::map<std::string, std::string> sizes = {
    { "A4", "A4" },
    { "A5", "A5" },
    { "A6", "A6" },
    { "B5", "B5" },
    { "B6", "B6" },
    { "EnvC5", "C5" },
    { "EnvMonarch", "MONARCH" },
    { "EnvPRC5", "DL" },
    { "EnvDL", "DL" },
    { "Executive", "EXECUTIVE" },
    { "Legal", "LEGAL" },
    { "Letter", "LETTER" }
  };

  page_params p = { };
  p.num_copies = header.NumCopies;
  p.resolution = header.HWResolution[0];
  p.economode = header.cupsInteger[10];
  p.mediatype = header.MediaType;
  p.duplex = header.Duplex;

  if (header.MediaPosition < sources.size())
    p.sourcetray = sources[header.MediaPosition];
  else
    p.sourcetray = sources[0];

  auto size_it = sizes.find(header.cupsPageSizeName);
  if (size_it != sizes.end())
    p.papersize = size_it->second;
  else
    p.papersize = "A4";

  return p;
}

}  // namespace



int main(int argc, char *argv[]) {
  fprintf(stderr, "INFO: %s version %s\n", PACKAGE, VERSION);

  if (argc != 6 && argc != 7) {
      fprintf(stderr, "ERROR: %s job-id user title copies options [file]\n", argv[0]);
      fprintf(stderr, "INFO: This program is a CUPS filter. It is not intended to be run manually.\n");
      return 1;
  }
  const char *job_id = argv[1];
  const char *job_user = argv[2];
  const char *job_name = argv[3];
  // const int job_copies = atoi(argv[4]);
  // const char *job_options = argv[5];
  const char *job_filename = argv[6];
  // const char *job_charset = getenv("CHARSET");

  setlocale(LC_ALL, "");

  signal(SIGTERM, sigterm_handler);
  signal(SIGPIPE, SIG_IGN);

  int fd = 0;
  if (job_filename) {
    fd = open(job_filename, O_RDONLY);
    if (fd < 0) {
      fprintf(stderr, "ERROR: " PACKAGE ": Unable to open raster file\n");
      return 1;
    }
  }

  ras = cupsRasterOpen(fd, CUPS_RASTER_READ);
  if (!ras) {
    fprintf(stderr, "DEBUG: " PACKAGE ": Cannot read raster data. Most likely an earlier filter in the pipeline failed.\n");
    return 1;
  }

  int pages = 0;
  {
    job job(stdout, ascii_job_name(job_id, job_user, job_name));
    while (!interrupted && cupsRasterReadHeader2(ras, &header)) {
      if (header.cupsBitsPerPixel != 1
          || header.cupsBitsPerColor != 1
          || header.cupsNumColors != 1
          || header.cupsBytesPerLine > 10000) {
        fprintf(stderr, "ERROR: " PACKAGE ": Page %d: Bogus raster data.\n", pages + 1);
        dump_page_header(header);
        return 1;
      }
      if (pages == 0) {
        fprintf(stderr, "DEBUG: " PACKAGE ": Page header of first page\n");
        dump_page_header(header);
      }
      job.encode_page(build_page_params(),
                      header.cupsHeight,
                      header.cupsBytesPerLine,
                      next_line);
      fprintf(stderr, "PAGE: %d %d\n", ++pages, header.NumCopies);
    }
  }

  if (pages == 0) {
    fprintf(stderr, "ERROR: " PACKAGE ": No pages were found.\n");
    return 1;
  }

  fflush(stdout);
  if (ferror(stdout)) {
    fprintf(stderr, "ERROR: " PACKAGE ": Could not write print data\n");
    return 1;
  }
  return 0;
}
