// Vitox 0.0.29.0: library settings + installing games / DLC from a USB drive (launcher Library "+" tile).
//
// Settings: LocalState\vitox-library.txt (show_homebrew=1|0, default 1): the library and the Artwork page list homebrew
// apps (the <dev_root>\homebrew root) only when it is on.
//
// Installer: .zip / .vpk archives on a removable drive (KnownFolders.RemovableDevices, package capability
// removableStorage + the .zip/.vpk file type association). Each archive is read in place (no copy to the console first)
// and every content root in it (a folder holding sce_sys\param.sfo, as upstream Vita3K's install_archive finds them) is
// classified by its param.sfo and installed where the Vitox launcher reads it:
//   game      (CATEGORY gd.., TITLE_ID PCS*)      -> <dev_root>\app-staging\<TITLE_ID>
//   homebrew  (CATEGORY gd.., any other TITLE_ID) -> <dev_root>\homebrew\<TITLE_ID> (or the folder that already has it)
//   update    (CATEGORY gp)                       -> merged into <dev_root>\app-staging\<TITLE_ID> (the game must be there)
//   DLC       (CATEGORY ac)                       -> <dev_root>\vita-fs\ux0\addcont\<TITLE_ID>\<CONTENT_ID[20:]>
// 0.0.29.1 (user-approved): NoNpDrm dumps (a license at sce_sys\package\work.bin) are decrypted while installing, the
// same steps as upstream Vita3K's decrypt_install_nonpdrm (psvpfsparser over BCrypt, core vitox_nonpdrm_decrypt), and
// the license is copied to vita-fs\ux0\license\<TITLE_ID>\<CONTENT_ID>.rif (upstream copy_license) where the launcher
// gives it to Vita3K's loader. Other encrypted content (no license) and Vitamin dumps are refused. Extraction goes to <dev_root>\.vitox-install first and is moved into place only when every file extracted
// (miniz checks each CRC); a replaced install is removed only after the new one is in place.
#pragma once

#include <cstdint>
#include <memory>
#include <string>
#include <vector>

namespace vitox {

// ---- settings ----
void library_load(const std::wstring &local_state);
bool library_show_homebrew();
void library_set_show_homebrew(bool on); // saved at once
// 0.1.1.0 (user request): the USB install folder (vitox-library.txt install_folder=). Empty = the whole drive is searched
// (every folder, as before); otherwise a path from the drive's root ("\" = the root itself, "\Vita\Games") and only that
// folder's own files are listed, on every USB drive that has it - the drive letter / name does not matter.
std::wstring library_install_folder();
void library_set_install_folder(const std::wstring &folder); // saved at once

namespace install {

// 0.1.1.0: browsing the USB drives' folders to choose the install folder (Settings > Library). All WinRT storage calls run
// on a worker thread; the UI polls a snapshot.
struct FolderView {
    bool loading = true;
    bool at_drives = true;             // the list holds the USB drives (the top level)
    std::wstring drive;                // the open drive's display name (at_drives = false)
    std::wstring path;                 // the open folder from the drive's root: "\" or "\Vita\Games"
    std::vector<std::wstring> entries; // drives (at_drives) or the open folder's sub-folders, sorted
    std::wstring message;              // "No USB drive found", an error, ...
    uint64_t version = 0;
};
class FolderBrowser {
public:
    FolderBrowser();
    ~FolderBrowser();
    void open_root();     // the list of USB drives
    void enter(size_t i); // open entries[i] (a drive or a sub-folder)
    void up();            // the parent folder (from a drive's root: the drive list)
    bool poll(uint64_t have, FolderView &out);

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

enum class Kind : int { Game, Homebrew, Update, Dlc };
const wchar_t *kind_name(Kind k);

struct ArchiveFile {
    std::wstring name;   // file name
    std::wstring drive;  // the drive's display name ("USB Drive (E:)")
    std::wstring folder; // the folder on the drive ("\" = the root)
    uint64_t size = 0;
};

struct Item {
    Kind kind = Kind::Game;
    std::wstring title, title_id, content_id, version, dest;
    uint64_t bytes = 0; // uncompressed
    unsigned files = 0;
    bool installable = true;
    bool replaces = false;    // already installed: replaced (an update: merged over the game)
    std::wstring problem;     // why it cannot be installed (installable = false)
    std::wstring note;        // a remark that does not block (e.g. the game of a DLC is not installed yet)
    // 0.0.29.1: a NoNpDrm dump (PFS-encrypted, license sce_sys/package/work.bin): decrypted while installing, as upstream
    // decrypt_install_nonpdrm; license_src = the license to use when the archive has none (an update: its game's .rif)
    bool nonpdrm = false;
    std::wstring license_src;
    // result
    bool done = false, ok = false;
    std::wstring result;
};

enum class Phase : int { Idle, Scanning, Files, Analyzing, Confirm, Installing, Done };

struct Snapshot {
    Phase phase = Phase::Idle;
    uint64_t version = 0;
    // Files
    std::vector<ArchiveFile> files;
    unsigned drives = 0;
    std::wstring folder;  // 0.1.1.0: the install folder the scan used ("" = the whole drive)
    std::wstring message; // scan / analysis / final message (errors included)
    // Confirm / Installing / Done
    std::wstring archive;
    std::vector<Item> items;
    uint64_t need_bytes = 0, free_bytes = 0;
    bool space_ok = true;
    uint64_t done_bytes = 0, total_bytes = 0;
    std::wstring current; // the file being extracted
    bool cancelling = false, cancelled = false;
    unsigned installed = 0, failed = 0;
    std::string select_title_id; // the first game / homebrew installed (the library selects it)
    // 0.0.30.0: after a complete installation the archive may be deleted from the USB drive (one button)
    bool can_delete_archive = false, deleting_archive = false, archive_deleted = false;
    std::wstring delete_error;
};

class Installer {
public:
    explicit Installer(std::wstring dev_root);
    ~Installer();
    void scan();              // -> Scanning -> Files (or Files with an empty list + message)
    void analyze(size_t file); // Files -> Analyzing -> Confirm (or Done with the reason)
    void back();              // Confirm / Done -> Files (the archive is closed)
    void install();           // Confirm -> Installing -> Done
    void delete_archive();    // 0.0.30.0: Done (every item installed) -> the installed archive is deleted from the USB drive
    void cancel();            // stops a scan / analysis / installation (an installation cleans up what it began)
    void close();             // -> Idle (waits for the worker)
    bool busy() const;
    // a copy of the state when it changed since `have` (true) - cheap to call every frame
    bool poll(uint64_t have, Snapshot &out);

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

std::wstring format_bytes(uint64_t b);
uint64_t dir_size(const std::wstring &dir); // every regular file below dir (0 when missing)

// 0.0.30.0: uninstall an app from the library (game menu). Removes, inside <dev_root> only:
//   <app_root>\<folder> (app_root homebrew | app-staging), the launcher's copy vita-fs\ux0\app\<TITLE_ID> (unless another
//   installed app has the same TITLE_ID: shared_title_id), its DLC vita-fs\ux0\addcont\<TITLE_ID> and NoNpDrm license
//   vita-fs\ux0\license\<TITLE_ID> (games), and with delete_saves vita-fs\ux0\user\00\savedata\<TITLE_ID>; plus the
//   custom artwork and per-game settings in LocalState. Empty result = success.
std::wstring uninstall(const std::wstring &dev_root, const std::wstring &local_state, const std::string &app_root, const std::string &folder,
    const std::string &title_id, bool shared_title_id, bool delete_saves, uint64_t *freed);

} // namespace install
} // namespace vitox
