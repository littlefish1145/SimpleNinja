#include "version.h"

#include <stdlib.h>

#include "util.h"

using namespace std;

// sninja tracks the reference ninja's feature set closely, so it keeps the
// same version number for build.ninja compatibility purposes.
const char* kNinjaVersion = "1.14.0.git";

void ParseVersion(const string& version, int* major, int* minor) {
  size_t end = version.find('.');
  *major = atoi(version.substr(0, end).c_str());
  *minor = 0;
  if (end != string::npos) {
    size_t start = end + 1;
    end = version.find('.', start);
    *minor = atoi(version.substr(start, end).c_str());
  }
}

void CheckNinjaVersion(const string& version, int* file_major, int* file_minor) {
  int bin_major, bin_minor;
  ParseVersion(kNinjaVersion, &bin_major, &bin_minor);
  ParseVersion(version, file_major, file_minor);

  if (bin_major > *file_major) {
    Warning("ninja executable version (%s) greater than build file "
            "ninja_required_version (%s); versions may be incompatible.",
            kNinjaVersion, version.c_str());
    return;
  }

  if ((bin_major == *file_major && bin_minor < *file_minor) ||
      bin_major < *file_major) {
    Fatal("ninja version (%s) incompatible with build file "
          "ninja_required_version version (%s).",
          kNinjaVersion, version.c_str());
  }
}
