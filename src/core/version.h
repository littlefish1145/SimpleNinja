#ifndef SNINJA_VERSION_H_
#define SNINJA_VERSION_H_

#include <string>

/// The version number of the current Ninja release.  This will always
/// be "git" on trunk.
extern const char* kNinjaVersion;

/// Parse the major/minor components of a version string.
void ParseVersion(const std::string& version, int* major, int* minor);

/// Check whether \a version is compatible with the current Ninja version,
/// aborting if not.
void CheckNinjaVersion(const std::string& required_version, int* file_major,
                       int* file_minor);

#endif  // SNINJA_VERSION_H_
