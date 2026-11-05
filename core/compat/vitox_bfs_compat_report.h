// Vitox: which Boost.Filesystem WindowsStore compatibility substitutes ran (vitox_bfs_compat.cpp).
#pragma once

struct VitoxBfsLookup {
    char name[40];
    bool resolved;
};

struct VitoxBfsCompatReport {
    unsigned create_file_calls;    // CreateFileW -> CreateFileFromAppW
    unsigned create_file_ok;       // returned a valid handle
    unsigned create_file_dev;      // ... for a path under D:\DevelopmentFiles
    unsigned create_file_local;    // ... for a path under the package LocalState
    unsigned long last_error_fail; // GetLastError of the most recent failed open (0 if none)
    unsigned are_file_apis_ansi;   // AreFileApisANSI -> TRUE
    unsigned get_windows_directory; // GetWindowsDirectoryW -> 0 / ERROR_NOT_SUPPORTED
    unsigned get_module_handle;    // boost::winapi::GetModuleHandleW
    unsigned lookups;              // boost::winapi::get_proc_address entries recorded below
    VitoxBfsLookup lookup[16];
};

extern "C" VitoxBfsCompatReport vitox_bfs_compat_report();
