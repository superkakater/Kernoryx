#include "sysfs.hpp"
#include <functional>
#include <iostream>
#include <stdexcept>
#include <unistd.h>
static int checks = 0;
static void check(bool ok, const char* label) { ++checks; if (!ok) throw std::runtime_error(label); }
static void fails(const std::function<void()>& f, const char* label) { bool error=false; try { f(); } catch (...) { error=true; } check(error,label); }
int main() {
    char pattern[] = "/tmp/kx-core-XXXXXX";
    auto tmp = mkdtemp(pattern); if (!tmp) return 1;
    filesystem::path base(tmp), root = base/"sys", journal = base/"rollback";
    auto put = [&](const std::string& path,const std::string& v) { filesystem::create_directories((root/path).parent_path()); kx::write(root/path,v); };
    const std::string profile="firmware/acpi/platform_profile", cpu="devices/system/cpu/cpufreq/policy0/", temp="class/hwmon/hwmon0/temp1_input", ac="class/power_supply/AC/online";
    try {
        put(profile,"balanced"); put("firmware/acpi/platform_profile_choices","low-power balanced performance");
        put(cpu+"scaling_driver","intel_pstate"); put(cpu+"scaling_governor","powersave");
        put(cpu+"scaling_available_governors","powersave performance");
        put(cpu+"energy_performance_preference","balance_power");
        put(cpu+"energy_performance_available_preferences","power balance_power balance_performance performance");
        put("devices/system/cpu/intel_pstate/status","active"); put("devices/system/cpu/intel_pstate/no_turbo","0");
        put("class/hwmon/hwmon0/name","coretemp"); put(temp,"55000"); put("class/power_supply/AC/type","Mains"); put(ac,"1");
        kx::Controller c(root,journal,true); auto original = c.backend.snapshot();
        c.set("beast",30,false,true);
        check(c.backend.snapshot()==original && !filesystem::exists(journal),"dry run mutates");
        c.set("beast",2,false,false);
        check(c.backend.get(cpu+"energy_performance_preference")=="performance","beast EPP");
        fails([&]{c.set("eco",0,false,false);},"active lease not protected");
        c.tick(c.deadline+1);
        check(c.requested=="unmanaged" && c.backend.snapshot()==original,"expiry restoration");
        c.set("balanced",0,false,false); c.set("beast",10,false,false); double oldDeadline=c.deadline;
        c.set("eco",0,true,false); c.tick(oldDeadline+1);
        check(c.requested=="eco" && c.backend.get(profile)=="low-power","stale lease overwrite");
        c.set("balanced",0,false,false); c.set("beast",10,false,false); c.set("performance",20,true,false); c.restore();
        check(c.requested=="balanced","replacement return target");
        c.set("beast",30,false,false); put(temp,"88000"); c.tick(kx::boottime());
        check(c.effective=="balanced","hot fallback");
        put(temp,"96000"); c.tick(kx::boottime());
        check(c.requested=="eco" && c.deadline==0 && c.policy.critical,"critical latch/cancel");
        fails([&]{c.set("beast",0,false,false);},"hot latch bypass");
        put(temp,"55000"); c.set("performance",0,false,false); put(ac,"0"); c.tick(kx::boottime());
        check(c.effective=="balanced","AC fallback");
        c.set("eco",5,false,false); put(ac,"1"); c.restore();
        check(c.effective=="performance" && c.backend.get(cpu+"energy_performance_preference")=="balance_performance","restore must re-resolve previously throttled mode");
        filesystem::remove(root/temp); c.tick(kx::boottime());
        check(c.effective=="balanced","missing sensor fallback");
        put(temp,"55000"); c.release(); check(c.backend.snapshot()==original,"release snapshot");
        c.set("beast",30,false,false); put(profile,"low-power");
        fails([&]{c.tick(kx::boottime());},"external write unnoticed");
        check(c.requested=="external-change","external state"); c.release();
        check(c.backend.get(profile)=="low-power","external write overwritten");
        c.backend.apply(original); c.set("beast",30,false,false);
        kx::Controller restarted(root,journal,true); restarted.recover();
        check(restarted.backend.snapshot()==original && !filesystem::exists(journal),"crash recovery");
        restarted.set("beast",30,false,false); put(profile,"low-power");
        kx::Controller conflict(root,journal,true);
        fails([&]{conflict.recover();},"recovery conflict accepted");
        check(filesystem::exists(journal),"conflict journal erased");
        kx::write(journal,"KERNORYX1\n\"firmware/acpi/platform_profile\" \"balanced\" \"low-power\" \"low-power\"\n\"truncated");
        fails([&]{conflict.recover();},"truncated journal accepted");
        check(conflict.backend.get(profile)=="low-power","truncated recovery mutated");
        filesystem::remove(journal); conflict.backend.apply(original);
        conflict.set("eco",0,false,false); filesystem::remove(root/(cpu+"energy_performance_preference"));
        fails([&]{conflict.tick(kx::boottime());},"topology drift missed");
        check(conflict.requested=="external-change","topology should suspend");
        std::cout << "Controller: " << checks << " assertions passed\n";
        filesystem::remove_all(base); return 0;
    } catch (const std::exception& e) { std::cerr << "Controller test failed: " << e.what() << '\n'; filesystem::remove_all(base); return 1; }
}
