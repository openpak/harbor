// SPDX-FileCopyrightText: 2026 tobagin
// SPDX-License-Identifier: AGPL-3.0-only

// harbor_setup_selftest: the setup's flow, record, device check, keys check and driver filter, without a
// window or a console. Exits 0 and prints "harbor_setup_selftest: all N checks passed" when they hold.

#include <algorithm>
#include <cstdio>
#include <cstddef>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <functional>
#include <string>
#include <vector>

#include "core/hle/service/set/setting_formats/system_settings.h"
#include "harbor_setup/chime.h"
#include "harbor_setup/device_check.h"
#include "harbor_setup/driver_catalog.h"
#include "harbor_setup/harbor_setup.h"
#include "harbor_setup/keys.h"
#include "harbor_setup/menu_input.h"
#include "harbor_setup/record.h"
#include "harbor_setup/wizard.h"

namespace {

using namespace HarborSetup;

int checks = 0;
int failures = 0;

void Check(bool ok, const std::string& what) {
    checks++;
    if (!ok) {
        failures++;
        std::printf("FAIL: %s\n", what.c_str());
    }
}

// A host whose pickers answer at once with prepared choices.
class FakeHost final : public Host {
public:
    std::vector<Picked> folders;
    bool driver_store = false;
    std::string selected_driver = "unset";

    bool IsAndroid() const override {
        return false;
    }
    DeviceInfo Probe() override {
        return SampleDevice("desktop");
    }
    void PickFile(const std::string&, const std::vector<std::string>&, PickCallback done) override {
        done(std::nullopt);
    }
    void PickFolder(const std::string&, PickCallback done) override {
        if (folders.empty()) {
            done(std::nullopt);
            return;
        }
        auto picked = folders.front();
        folders.erase(folders.begin());
        done(picked);
    }
    bool HasDriverStore() const override {
        return driver_store;
    }
    bool SelectDriver(const std::string& id, std::string*) override {
        selected_driver = id;
        return true;
    }
    int http_gets = 0;
    std::optional<std::string> HttpGet(const std::string&, std::uint64_t, const std::atomic_bool&,
                                       std::string* error) override {
        http_gets++;
        if (error) {
            *error = "offline";
        }
        return std::nullopt;
    }
};

std::string Release(const std::string& name, const std::string& tag, const std::string& body,
                    const std::string& repository, const std::vector<std::string>& assets,
                    const std::string& date) {
    std::string json = "{\"name\":\"" + name + "\",\"tag_name\":\"" + tag + "\",\"body\":\"" + body +
                       "\",\"draft\":false,\"prerelease\":false,\"published_at\":\"" + date +
                       "T00:00:00Z\",\"assets\":[";
    for (size_t i = 0; i < assets.size(); i++) {
        json += std::string(i ? "," : "") + "{\"name\":\"" + assets[i] +
                "\",\"browser_download_url\":\"https://github.com/" + repository + "/releases/download/" +
                tag + "/" + assets[i] + "\",\"size\":3000000,\"digest\":\"sha256:" + std::string(64, 'a') +
                "\"}";
    }
    return json + "]}";
}

bool Offers(const std::vector<DriverRelease>& releases, const std::string& file) {
    return std::any_of(releases.begin(), releases.end(),
                       [&](const DriverRelease& r) { return r.file_name == file; });
}

void TestRecord(const std::filesystem::path& dir) {
    Record record;
    record.keys_present = true;
    record.firmware_version = "23.0.1";
    record.game_dirs = std::vector<std::string>{"/games"};
    record.dlc_dirs = std::vector<std::string>{};
    record.update_dirs = std::vector<std::string>{"/updates", "/updates"};
    const auto parsed = FromJson(ToJson(record));
    Check(parsed.has_value(), "record: JSON round trip parses");
    Check(parsed && parsed->dlc_dirs && parsed->dlc_dirs->empty(), "record: [] stays [] (page passed, nothing chosen)");
    Check(parsed && !parsed->gpu_driver, "record: null stays null (page not passed)");
    Check(record.AutoloadDirs() == std::vector<std::string>{"/updates"}, "record: autoload folders without duplicates");
    Check(ToJson(record).find("\"keys_present\": true") != std::string::npos, "record: snake_case names");

    const auto path = dir / "harbor-setup.json";
    Check(SaveRecord(record, path), "record: saved");
    Check(!std::filesystem::exists(dir / "harbor-setup.json.tmp"), "record: no .tmp left behind");
    Check(LoadRecord(path).firmware_version == std::optional<std::string>{"23.0.1"}, "record: loads back");
    std::ofstream(path) << "{ broken";
    Check(!LoadRecord(path).keys_present, "record: a broken file starts setup again");
}

void TestResume() {
    Readiness none;
    Record record;
    Check(Wizard::FirstIncomplete(none, record, false) == Step::Welcome, "resume: no keys -> Welcome");
    Readiness keys;
    keys.keys_usable = true;
    Check(Wizard::FirstIncomplete(keys, record, false) == Step::Firmware, "resume: no firmware -> Firmware");
    keys.qlaunch = true;
    Check(Wizard::FirstIncomplete(keys, record, false) == Step::Firmware,
          "resume: qlaunch without a readable version -> Firmware");
    keys.firmware_version = "23.0.1";
    Check(Wizard::FirstIncomplete(keys, record, false) == Step::Games, "resume: no games -> Games");
    record.game_dirs = std::vector<std::string>{"/g"};
    Check(Wizard::FirstIncomplete(keys, record, false) == Step::Dlc, "resume: dlc null -> DLC");
    record.dlc_dirs = std::vector<std::string>{};
    record.update_dirs = std::vector<std::string>{};
    Check(Wizard::FirstIncomplete(keys, record, true) == Step::Driver, "resume: driver null -> Driver (shown)");
    Check(Wizard::FirstIncomplete(keys, record, false) == Step::Account,
          "resume: driver page hidden, account page not passed -> OpenPak account");
    record.openpak_account = "later";
    Check(Wizard::FirstIncomplete(keys, record, false) == Step::Ready, "resume: account passed -> Ready");
}

void TestDeviceCheck() {
    Check(InstalledGb(3700000000LL) == 4, "device: 3.7e9 bytes is 4 GB installed");
    Check(InstalledGb(12LL << 30) == 12, "device: 12 GiB is 12 GB");
    const auto blocked = RunDeviceCheck(SampleDevice("blocked"));
    Check(std::any_of(blocked.begin(), blocked.end(),
                      [](const auto& r) { return r.title == "Processor" && r.level == CheckLevel::Fail; }),
          "device: armeabi-v7a fails the processor check");
    const auto warn = RunDeviceCheck(SampleDevice("warn"));
    Check(std::none_of(warn.begin(), warn.end(), [](const auto& r) { return r.level == CheckLevel::Fail; }) &&
              std::count_if(warn.begin(), warn.end(), [](const auto& r) { return r.level == CheckLevel::Warn; }) == 3,
          "device: the warn sample warns on memory, driver and storage");
    Check(RunDeviceCheck(SampleDevice("desktop")).size() == 3, "device: desktop shows processor, memory, storage");
}

void TestKeysCheck() {
    const std::string header = "header_key = " + std::string(64, '0') + "\n";
    Check(KeysFileProblem(header + "key_area_key_application_00 = " + std::string(32, '1') + "\n", false).empty(),
          "keys: a well-formed prod.keys passes");
    Check(!KeysFileProblem("master_key_00 = " + std::string(32, '1') + "\n", false).empty(),
          "keys: no header_key fails");
    Check(!KeysFileProblem("this is not a keys file\n", false).empty(), "keys: text fails");
    Check(!KeysFileProblem("header_key = 00112233\n", false).empty(), "keys: a short header_key fails");
    Check(KeysFileProblem(std::string(32, 'a') + " = " + std::string(32, 'b') + "\n", true).empty(),
          "keys: a well-formed title.keys passes");
}

void TestDriverCatalog() {
    DeviceInfo a650;
    a650.abis = std::vector<std::string>{"arm64-v8a"};
    a650.gpu = "Adreno (TM) 650";
    a650.adreno = true;
    a650.android_api = 31;
    DeviceInfo a740 = a650;
    a740.gpu = "Turnip Adreno (TM) 740";

    Check(AdrenoModel(a650) == 650 && AdrenoModel(a740) == 740, "drivers: Adreno model from the GPU name");
    Check(CanBrowseDrivers(a650), "drivers: an Adreno 650 on Android 12 may browse");

    const std::string kimchi = "K11MCH1/AdrenoToolsDrivers";
    const std::string kimchi_json =
        "[" + Release(" v25.3.0 - Revision 11", "v25.3.0-rc.11", "Vulkan 1.4.328", kimchi, {"Turnip_v25.3.0_R11.zip"}, "2025-10-09") +
        "," + Release("Qualcomm Driver v840", "v840", "For a7xx. Maybe a6xx.", kimchi, {"Qualcomm_840_adpkg.zip"}, "2026-01-14") + "]";
    const auto kimchi_650 = ParseDriverReleases(kimchi_json, kimchi, a650);
    Check(!Offers(kimchi_650, "Turnip_v25.3.0_R11.zip"), "drivers: KIMCHI's ad07xx Turnip is not offered on an Adreno 650");
    Check(!Offers(kimchi_650, "Qualcomm_840_adpkg.zip"), "drivers: 'maybe a6xx' is not a support claim");
    Check(Offers(ParseDriverReleases(kimchi_json, kimchi, a740), "Turnip_v25.3.0_R11.zip"),
          "drivers: KIMCHI's ad07xx Turnip is offered on an Adreno 740");

    const std::string purple = "MrPurple666/purple-turnip";
    const std::string purple_json =
        "[" + Release("turnip_mrpurple_T29-toasted.adpkg", "vturnip_mrpurple_T29-toasted.adpkg", "", purple,
                      {"turnip_mrpurple_T29-toasted.adpkg.zip"}, "2026-07-09") +
        "," + Release("turnip_mrpurple_T30-toasted.adpkg", "vturnip_mrpurple_T30-toasted.adpkg", "", purple,
                      {"turnip_mrpurple_T30-toasted.adpkg.zip"}, "2026-08-17") + "]";
    DriverListing listing;
    listing.releases = ParseDriverReleases(purple_json, purple, a650);
    const std::string steven = "StevenMXZ/Adreno-Tools-Drivers";
    const auto steven_releases = ParseDriverReleases(
        "[" + Release("Turnip Gen8 V37", "v37", "for A8xx GPUs", steven, {"Turnip_Gen8_V37.zip", "Turnip_v26.3.0-R6.zip"}, "2026-09-30") + "]",
        steven, a650);
    Check(!Offers(steven_releases, "Turnip_Gen8_V37.zip") && Offers(steven_releases, "Turnip_v26.3.0-R6.zip"),
          "drivers: gen8 packages only for Adreno 8xx");
    listing.releases.insert(listing.releases.begin(), steven_releases.begin(), steven_releases.end());
    RankDriverListing(listing, a650);
    Check(!listing.releases.empty() && listing.releases.front().file_name == "turnip_mrpurple_T30-toasted.adpkg.zip" &&
              listing.releases.front().recommended,
          "drivers: MrPurple T30 comes first and is recommended on an Adreno 6xx");
    Check(listing.releases.size() == 3 && listing.releases[1].file_name == "Turnip_v26.3.0-R6.zip" &&
              listing.releases[2].file_name == "turnip_mrpurple_T29-toasted.adpkg.zip",
          "drivers: the rest keep their order");
    DeviceInfo old_android = a650;
    old_android.android_api = 29;
    Check(ParseDriverReleases(purple_json, purple, old_android).empty(), "drivers: MrPurple needs Android 11");

    Check(LibraryFamily("vulkan.ad07xx.so") == 7 && !LibraryFamily("vulkan.purple.so"),
          "drivers: library names give the Adreno generation");
    Check(!DriverPackageProblem("Mesa Turnip", "vulkan.ad07xx.so", 27, a650).empty(),
          "drivers: an ad07xx library is refused on an Adreno 650");
    Check(DriverPackageProblem("Mesa Turnip", "vulkan.ad07xx.so", 27, a740).empty(),
          "drivers: an ad07xx library is fine on an Adreno 740");
    Check(!DriverPackageProblem("Turnip T30", "vulkan.purple.so", 36, a650).empty(),
          "drivers: minApi above the phone's API is refused");
    Check(AllowedDriverDownload("https://github.com/" + purple + "/releases/download/t/x.zip") &&
              !AllowedDriverDownload("https://github.com/" + purple + "/releases/download/../../evil/x.zip") &&
              !AllowedDriverDownload("https://example.com/" + purple + "/releases/download/t/x.zip"),
          "drivers: only the publishers' GitHub release downloads");
    Check(ParseDriverReleases("not json", purple, a650).empty(), "drivers: broken JSON gives nothing");
}

void TestFlow(const std::filesystem::path& dir) {
    FakeHost host;
    host.folders.push_back({"Switch", "/home/me/Games/Switch", true});
    Readiness readiness;
    readiness.keys_usable = true;
    readiness.qlaunch = true;
    readiness.firmware_version = "23.0.1";
    const auto path = dir / "flow.json";
    std::filesystem::remove(path);

    int notes = 0;
    bool finished = false;
    Wizard wizard(host, path, readiness, SampleDevice("desktop"), [](std::function<void()> f) { f(); });
    wizard.page_shown = [&](int) { notes++; };
    wizard.finished = [&] { finished = true; };

    Check(wizard.step() == Step::Games, "flow: a resume with keys and firmware opens at Games");
    Check(!wizard.CanGoNext(), "flow: Games needs a folder");
    wizard.Next();
    Check(!std::filesystem::exists(path), "flow: nothing recorded before a Next succeeds");
    wizard.AddFolder();
    Check(wizard.Folders().size() == 1 && wizard.Folders()[0].name == "Switch", "flow: the picked folder is listed");
    wizard.Next();
    Check(wizard.step() == Step::Dlc && LoadRecord(path).game_dirs == std::optional{std::vector<std::string>{"/home/me/Games/Switch"}},
          "flow: Next on Games records game_dirs");
    Check(wizard.NextLabel() == "Skip" && !LoadRecord(path).dlc_dirs, "flow: DLC says Skip and is still null");
    wizard.Back();
    Check(wizard.step() == Step::Games, "flow: Back returns");
    wizard.Next();
    wizard.Next();
    Check(LoadRecord(path).dlc_dirs == std::optional{std::vector<std::string>{}}, "flow: skipped DLC is []");
    wizard.Next();
    Check(wizard.step() == Step::Account, "flow: no driver page on desktop; the OpenPak account page");
    Check(!wizard.CanGoNext() && wizard.NextLabel() == "Sign in", "flow: Sign in needs an email and a password");
    wizard.SetAccountField(AccountField::Email, " someone@example.org ");
    wizard.SetAccountField(AccountField::Password, "short");
    Check(wizard.CanGoNext() && wizard.AccountValue(AccountField::Email) == "someone@example.org",
          "flow: account fields are kept, trimmed");
    wizard.Next();
    Check(wizard.step() == Step::Account && wizard.StatusTone() == Tone::Bad,
          "flow: a short password is refused before anything is sent");
    wizard.SetAccountMode(AccountMode::Create);
    Check(!wizard.CanGoNext() && wizard.NextLabel() == "Create account", "flow: Create needs every field");
    wizard.SetAccountField(AccountField::Password, "long enough password");
    wizard.SetAccountField(AccountField::Nickname, "Tester");
    wizard.SetAccountField(AccountField::Birthday, "1990-02-30");
    wizard.SetAccountField(AccountField::Country, "gb");
    wizard.Next();
    Check(wizard.step() == Step::Account && wizard.StatusTone() == Tone::Bad,
          "flow: an impossible birthday is refused before anything is sent");
    Check(wizard.AccountValue(AccountField::Country) == "GB", "flow: the country is upper case");
    Check(wizard.CanSkip() && wizard.SkipLabel() == "Later", "flow: the account page says Later");
    wizard.Skip();
    Check(wizard.step() == Step::Ready && LoadRecord(path).openpak_account == std::optional<std::string>{"later"},
          "flow: Later records later and goes on");
    Check(!LoadRecord(path).completed, "flow: not completed before Start Harbor");
    Check(wizard.NextLabel() == "Start Harbor", "flow: Ready says Start Harbor");
    wizard.Next();
    Check(finished && LoadRecord(path).completed, "flow: Start Harbor records completed and finishes");
    Check(notes == 6, "flow: one cue per page shown");
}

// G4.WIZ.20, G4.WIZ.21 and G4.GPU.3: a picker that fails, Back on Welcome, no GitHub request
// before "Browse GitHub drivers".
void TestPickerBackAndDriverPage(const std::filesystem::path& dir) {
    {
        FakeHost host;
        Wizard wizard(host, dir / "welcome.json", Readiness{}, SampleDevice("desktop"),
                      [](std::function<void()> f) { f(); });
        Check(wizard.step() == Step::Welcome && wizard.OnWelcome(),
              "back: a new setup is on Welcome, where Android back leaves the app");
    }
    {
        FakeHost host;
        Readiness readiness;
        readiness.keys_usable = true;
        readiness.qlaunch = true;
        readiness.firmware_version = "23.0.1";
        Wizard wizard(host, dir / "picker.json", readiness, SampleDevice("desktop"),
                      [](std::function<void()> f) { f(); });
        Check(wizard.step() == Step::Games && !wizard.OnWelcome(), "back: past Welcome, back is the wizard's Back");
        wizard.PickerFailed("No Activity found to handle Intent");
        Check(wizard.Status() == "The picker could not open: No Activity found to handle Intent" &&
                  wizard.StatusTone() == Tone::Bad && wizard.Folders().empty(),
              "picker: a picker that throws says so and changes nothing");
    }
    {
        FakeHost host;
        host.driver_store = true;
        DeviceInfo a650;
        a650.abis = std::vector<std::string>{"arm64-v8a"};
        a650.gpu = "Adreno (TM) 650";
        a650.adreno = true;
        a650.android_api = 31;
        Record record;
        record.keys_present = true;
        record.firmware_version = "23.0.1";
        record.game_dirs = std::vector<std::string>{"/games"};
        record.dlc_dirs = std::vector<std::string>{};
        record.update_dirs = std::vector<std::string>{};
        SaveRecord(record, dir / "driver.json");
        Readiness readiness;
        readiness.keys_usable = true;
        readiness.qlaunch = true;
        readiness.firmware_version = "23.0.1";
        Wizard wizard(host, dir / "driver.json", readiness, a650, [](std::function<void()> f) { f(); });
        Check(wizard.step() == Step::Driver && host.http_gets == 0 && !wizard.Busy(),
              "drivers: the driver page on an Adreno 650 sends nothing to GitHub until Browse");
    }
}

// G4.CON.5: the host theme is the console's saved colour set, the platform's before a boot.
void TestConsoleTheme(const std::filesystem::path& dir) {
    const auto path = dir / "settings.dat";
    Check(!ReadConsoleDarkTheme(path), "theme: no settings file follows the platform");
    const auto write = [&](unsigned char color) {
        std::string bytes(16 + sizeof(Service::Set::SystemSettings), '\0');
        const char magic[] = "yuzu_set";
        std::memcpy(bytes.data(), magic, 8);
        bytes[8] = 4;
        bytes[16 + offsetof(Service::Set::SystemSettings, color_set_id)] = static_cast<char>(color);
        std::ofstream(path, std::ios::binary).write(bytes.data(), static_cast<std::streamsize>(bytes.size()));
    };
    write(1);
    Check(ReadConsoleDarkTheme(path) == std::optional<bool>{true}, "theme: Basic Black is dark");
    write(0);
    Check(ReadConsoleDarkTheme(path) == std::optional<bool>{false}, "theme: Basic White is light");
    Check(ReadInitialSetupDone(path) == std::optional<bool>{false}, "dock: first-time setup not done yet");
    {
        std::fstream file(path, std::ios::binary | std::ios::in | std::ios::out);
        file.seekp(16 + offsetof(Service::Set::SystemSettings, initial_launch_settings_packed));
        const char done = 1;
        file.write(&done, 1);
    }
    Check(ReadInitialSetupDone(path) == std::optional<bool>{true}, "dock: first-time setup done");
    Check(!ReadInitialSetupDone(dir / "missing.dat"), "dock: no settings file, no answer");
}

void TestChime() {
    const auto samples = Chime::Samples(0);
    Check(samples.size() == static_cast<size_t>(Chime::SampleRate * Chime::DurationMs / 1000), "chime: 180 ms");
    int peak = 0;
    for (const auto s : samples) {
        peak = std::max(peak, std::abs(static_cast<int>(s)));
    }
    Check(peak > 1000 && peak < 32767 / 3, "chime: audible, under a third of full scale");
    Check(std::abs(samples.front()) < 50 && std::abs(samples.back()) < 50, "chime: starts and ends silently");
}

} // namespace

static void TestInputStore(const std::filesystem::path& dir) {
    const auto path = dir / "harbor-input.json";
    Common::ParamPackage pad{{"engine", "sdl"}, {"guid", "030000005e0400008e02000014010000"}, {"port", "1"},
                             {"display", "Xbox pad 1"}};
    Common::ParamPackage keyboard{{"engine", "keyboard"}, {"display", "Keyboard"}};
    Check(InputStore::DeviceKey(pad) == "sdl:030000005e0400008e02000014010000", "input: a pad is kept by GUID");
    Check(InputStore::DeviceKey(keyboard) == "keyboard", "input: the keyboard has one entry");
    {
        InputStore store(path);
        InputStore::Profile profile;
        profile.buttons[0] = "engine:sdl,guid:030000005e0400008e02000014010000,port:1,button:1";
        profile.analogs[0] = "engine:sdl,guid:030000005e0400008e02000014010000,port:1,axis_x:0,axis_y:1";
        store.Put(InputStore::DeviceKey(pad), profile);
        store.SetPower("engine:keyboard,code:80,toggle:0");
        Check(store.Save(), "input: store saved");
    }
    InputStore loaded(path);
    const auto profile = loaded.Find(InputStore::DeviceKey(pad));
    Check(profile.has_value() && profile->buttons[0].find("button:1") != std::string::npos,
          "input: a pad's mapping loads back");
    Check(loaded.Power() == "engine:keyboard,code:80,toggle:0", "input: the Power binding loads back");
    Check(!loaded.Find("sdl:other").has_value(), "input: an unknown pad has no saved mapping");

    // The same pad on another port gets its mapping moved there.
    Common::ParamPackage moved_pad = pad;
    moved_pad.Set("port", 3);
    const Common::ParamPackage moved{RetargetParam(profile->buttons[0], moved_pad)};
    Check(moved.Get("port", 0) == 3 && moved.Get("button", 0) == 1, "input: a saved mapping follows the pad's port");
    const Common::ParamPackage keys{RetargetParam(
        "engine:analog_from_button,up:engine$0sdl$1guid$0030000005e0400008e02000014010000$1port$01$1button$03,"
        "modifier_scale:0.5",
        moved_pad)};
    Check(Common::ParamPackage{keys.Get("up", "")}.Get("port", 0) == 3, "input: a stick from buttons follows too");
    Check(RetargetParam("engine:keyboard,code:65", moved_pad) == "engine:keyboard,code:65",
          "input: another device's binding is left alone");
}

int main() {
    const char* tmp = std::getenv("TMPDIR");
    const auto dir = std::filesystem::path(tmp ? tmp : ".") / "harbor-setup-selftest";
    std::filesystem::create_directories(dir);

    TestRecord(dir);
    TestResume();
    TestDeviceCheck();
    TestKeysCheck();
    TestDriverCatalog();
    TestFlow(dir);
    TestPickerBackAndDriverPage(dir);
    TestConsoleTheme(dir);
    TestChime();
    TestInputStore(dir);

    std::filesystem::remove_all(dir);
    if (failures) {
        std::printf("harbor_setup_selftest: %d of %d checks failed\n", failures, checks);
        return 1;
    }
    std::printf("harbor_setup_selftest: all %d checks passed\n", checks);
    return 0;
}
