/*
main.c - RVVM CLI, API usage example
Copyright (C) 2021  LekKit <github.com/LekKit>
                    cerg2010cerg2010 <github.com/cerg2010cerg2010>
                    Mr0maks <mr.maks0443@gmail.com>
                    KotB <github.com/0xCatPKG>
                    fish4terrisa-MSDSM <fish4terrisa@fishinix.eu.org>
                    David Korenchuk <github.com/epoll-reactor-2>

This program is free software: you can redistribute it and/or modify
it under the terms of the GNU General Public License as published by
the Free Software Foundation, either version 3 of the License, or
(at your option) any later version.

This program is distributed in the hope that it will be useful,
but WITHOUT ANY WARRANTY; without even the implied warranty of
MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
GNU General Public License for more details.

You should have received a copy of the GNU General Public License
along with this program.  If not, see <https://www.gnu.org/licenses/>.
*/

#include "feature_test.h"

#include "gdbstub.h"
#include "rvvm_isolation.h"
#include "rvvm_user.h"
#include "rvvmlib.h"
#include "utils.h"

#include "devices/ata.h"
#include "devices/bochs-display.h"
#include "devices/framebuffer.h"
#include "devices/i2c-oc.h"
#include "devices/ns16550a.h"
#include "devices/nvme.h"
#include "devices/pci-bus.h"
#include "devices/pci-vfio.h"
#include "devices/riscv-aclint.h"
#include "devices/riscv-aplic.h"
#include "devices/riscv-imsic.h"
#include "devices/riscv-plic.h"
#include "devices/rtc-goldfish.h"
#include "devices/rtl8169.h"
#include "devices/sound-hda.h"
#include "devices/syscon.h"
#include "devices/usb-xhci.h"

#include "gui/gui_window.h"

PUSH_OPTIMIZATION_SIZE

#if defined(HOST_TARGET_WINNT)

// For WriteFile(), GetCommandLine(), SetConsoleOutputCP(), SetConsoleCP()
#include <windows.h>

// Split command line or environment string into argv/envp
static int split_cmdline(const char* cmdline, int argc, char*** argv_p, bool env)
{
    int end = 0, pos = 0, cnt = 0, esc = 0;
    if (cmdline) {
        do {
            if (cmdline[pos] == '\"' && !env) {
                esc = !esc;
            } else if (!cmdline[pos] || (cmdline[pos] == ' ' && !esc && !env)) {
                int len = pos - end;
                if (cmdline[end] != ' ' && len) {
                    if (argv_p && cnt < argc) {
                        (*argv_p)[cnt] = safe_new_arr(char, len + 1);
                        memcpy((*argv_p)[cnt], cmdline + end, len);
                    }
                    cnt++;
                }
                end += len + 1;
            }
        } while (cmdline[pos++] || (env && cmdline[pos]));
    }
    return cnt;
}

// Fill argc/argv/envp
static int fill_argv_envp(int* argc_p, char*** argv_p, char*** envp_p)
{
    static int    argc_g = 0;
    static char** argv_g = NULL;
    static char** envp_g = NULL;
    if (!argv_g) {
        // Get UTF-16 cmdline & convert to UTF-8, fallback to ANSI
        LPWSTR cmdline_u16 = GetCommandLineW();
        char*  cmdline     = cmdline_u16 ? utf16_to_utf8(cmdline_u16) : GetCommandLineA();
        // Split cmdline into argv
        argc_g = split_cmdline(cmdline, 0, NULL, false);
        argv_g = safe_new_arr(char*, argc_g + 1);
        split_cmdline(cmdline, argc_g, &argv_g, false);
        // Free UTF-8 cmdline if needed
        if (cmdline_u16) {
            free(cmdline);
        }
        if (!cmdline) {
            return -1;
        }
    }
    if (!envp_g) {
        const char* envstr = GetEnvironmentStringsA();
        // Split environment string into envp
        int envc = split_cmdline(envstr, 0, NULL, true);
        envp_g   = safe_new_arr(char*, envc + 1);
        split_cmdline(envstr, envc, &envp_g, true);
        if (!envstr) {
            return -1;
        }
    }
    if (argc_p) {
        *argc_p = argc_g;
    }
    if (argv_p) {
        *argv_p = argv_g;
    }
    if (envp_p) {
        *envp_p = envp_g;
    }
    return 0;
}

static void print_stderr(const char* str)
{
#if defined(HOST_TARGET_WINNT)
    DWORD count = 0;
    while (str[count]) {
        count++;
    }
    WriteFile(GetStdHandle(STD_ERROR_HANDLE), str, count, &count, NULL);
#else
    fputs(str, stderr);
#endif
}

#if defined(GNU_EXTS) && defined(HOST_32BIT)

// Fix crtdll crash
int __getmainargs(int* argc_p, char*** argv_p, char*** envp_p, int do_wildcard, void* start_info)
{
    UNUSED(do_wildcard);
    UNUSED(start_info);
    return fill_argv_envp(argc_p, argv_p, envp_p);
}

#endif

#else

// For fputs()
#include <stdio.h>

static void print_stderr(const char* str)
{
    fputs(str, stderr);
}

#endif

static void rvvm_print_help(void)
{
    const char* help = /**/
        "\n"
        "  ██▀███   ██▒   █▓ ██▒   █▓ ███▄ ▄███▓\n"
        " ▓██ ▒ ██▒▓██░   █▒▓██░   █▒▓██▒▀█▀ ██▒\n"
        " ▓██ ░▄█ ▒ ▓██  █▒░ ▓██  █▒░▓██    ▓██░\n"
        " ▒██▀▀█▄    ▒██ █░░  ▒██ █░░▒██    ▒██ \n"
        " ░██▓ ▒██▒   ▒▀█░     ▒▀█░  ▒██▒   ░██▒\n"
        " ░ ▒▓ ░▒▓░   ░ ▐░     ░ ▐░  ░ ▒░   ░  ░\n"
        "   ░▒ ░ ▒░   ░ ░░     ░ ░░  ░  ░      ░\n"
        "   ░░   ░      ░░       ░░  ░      ░   \n"
        "    ░           ░        ░         ░   \n"
        "               ░        ░              \n"
        "\n"
        "https://github.com/LekKit/RVVM (" RVVM_VERSION ")\n"
        "\n"
        "License GPLv3+: GNU GPL version 3 or later <http://gnu.org/licenses/gpl.html>\n"
        "This is free software: you are free to change and redistribute it.\n"
        "There is NO WARRANTY, to the extent permitted by law.\n"
        "\n"
        "Usage: rvvm <firmware> [-m 256M] [-k kernel] [-i drive.img] ...\n"
        "\n"
        "    <firmware>       Initial M-mode firmware (OpenSBI [+ U-Boot], etc)\n"
        "    -k, -kernel ...  Optional S-mode kernel payload (Linux, U-Boot, etc)\n"
        "    -i, -image  ...  Attach preferred storage image (Currently as NVMe)\n"
        "    -m, -mem 1G      Memory amount, default: 256M\n"
        "    -s, -smp 4       Cores count, default: 1\n"
        "    -rv32            Enable 32-bit RISC-V, 64-bit by default\n"
        "    -cmdline    ...  Override payload kernel command line\n"
        "    -append     ...  Modify payload kernel command line\n"
        "    -res 1280x720    Set display(s) resolution\n"
        "    -poweroff_key    Send HID_KEY_POWER instead of exiting on GUI close\n"
        "    -portfwd 8080=80 Port forwarding (Extended: tcp/127.0.0.1:8080=80)\n"
        "    -vfio_pci   ...  PCI passthrough via VFIO (Example: 00:02.0), needs root\n"
        "    -nvme       ...  Explicitly attach storage image as NVMe device\n"
        "    -ata        ...  Explicitly attach storage image as ATA (IDE) device\n"
        "    -nogui           Disable display GUI\n"
        "    -nosound         Disable sound support\n"
        "    -nonet           Disable networking\n"
        "    -serial     ...  Add more serial ports (Via pty/pipe path), or null\n"
        "    -dtb        ...  Pass custom Device Tree Blob to the machine\n"
        "    -dumpdtb    ...  Dump auto-generated DTB to file\n"
        "    -v, -verbose     Enable verbose logging\n"
        "    -h, -help        Show this help message\n"
        "\n"
        "    -noisolation     Disable seccomp/pledge isolation\n"
        "    -nojit           Disable RVJIT (For debug purposes, slow!)\n"
        "\n";
    print_stderr(help);
}

static bool rvvm_cli_configure(rvvm_machine_t* machine, const char* bios, tap_dev_t* tap)
{
    UNUSED(tap);

    if (rvvm_has_arg("k") || rvvm_has_arg("kernel")) {
        // If we are booting a static kernel, append root device cmdline
        rvvm_append_cmdline(machine, "root=/dev/nvme0n1 rootflags=discard rw");
    }
    if (!rvvm_load_firmware(machine, bios)) {
        return false;
    }
    if (rvvm_getarg("k") && !rvvm_load_kernel(machine, rvvm_getarg("k"))) {
        return false;
    }
    if (rvvm_getarg("kernel") && !rvvm_load_kernel(machine, rvvm_getarg("kernel"))) {
        return false;
    }
    if (rvvm_getarg("dtb") && !rvvm_load_fdt(machine, rvvm_getarg("dtb"))) {
        return false;
    }

    int         arg_iter = 1;
    const char* arg_name = NULL;
    const char* arg_val  = NULL;
    while ((arg_name = rvvm_next_arg(&arg_val, &arg_iter))) {
        if (arg_val) {
            if (rvvm_strcmp(arg_name, "i") || rvvm_strcmp(arg_name, "image") || rvvm_strcmp(arg_name, "nvme")) {
                if (!nvme_init_auto(machine, arg_val, true)) {
                    rvvm_error("Failed to attach image \"%s\"", arg_val);
                    return false;
                }
            } else if (rvvm_strcmp(arg_name, "ata")) {
                if (!ata_init_auto(machine, arg_val, true)) {
                    rvvm_error("Failed to attach image \"%s\"", arg_val);
                    return false;
                }
            } else if (rvvm_strcmp(arg_name, "serial")) {
                chardev_t* chardev = chardev_pty_create(arg_val);
                if (chardev == NULL && !rvvm_strcmp(arg_val, "null")) {
                    return false;
                }
                ns16550a_init_auto(machine, chardev);
            } else if (rvvm_strcmp(arg_name, "res")) {
                size_t    len = 0;
                rvvm_fb_t fb  = {
                     .width  = str_to_uint_base(arg_val, &len, 10),
                     .height = str_to_uint_base(arg_val + len + 1, NULL, 10),
                     .format = RVVM_RGB_XRGB8888,
                };
                if (arg_val[len] == 'x' && rvvm_fb_size(&fb)) {
                    rvvm_simplefb_init_auto(machine, gui_window_get_fbdev(gui_rvvm_init(0, &fb, machine)));
                } else {
                    rvvm_error("Invalid resolution: %s, expects WxH", arg_val);
                    return false;
                }
#if defined(USE_NET)
            } else if (tap && rvvm_strcmp(arg_name, "portfwd")) {
                if (!tap_portfwd(tap, arg_val)) {
                    return false;
                }
#endif
            } else if (rvvm_strcmp(arg_name, "vfio_pci")) {
                if (!pci_vfio_init_auto(machine, arg_val)) {
                    return false;
                }
            }
        }
    }
    return true;
}

static int rvvm_cli_main(int argc, char** argv)
{
    // Set up argparser
    rvvm_set_args(argc, argv);
    if (rvvm_has_arg("h") || rvvm_has_arg("help") || rvvm_has_arg("H")) {
        rvvm_print_help();
        return 0;
    }

    // Default machine parameters: 1 core, 256M ram, riscv64, 640x480 screen
    size_t mem = rvvm_getarg_size("m");
    if (!mem) {
        mem = rvvm_getarg_size("mem");
    }
    if (!mem) {
        mem = (256 << 20);
    }

    size_t smp = rvvm_getarg_int("s");
    if (!smp) {
        smp = rvvm_getarg_int("smp");
    }
    if (!smp) {
        smp = 1;
    }

    const char* bios = rvvm_getarg("");
    if (!bios) {
        bios = rvvm_getarg("bios");
    }

    const char* isa = rvvm_getarg("isa");
    if (!isa) {
        isa = rvvm_has_arg("rv32") ? "rv32" : "rv64";
    }

    if (!bios) {
        // No firmware passed
        print_stderr("Usage: rvvm <firmware> [-mem 256M] [-k kernel] [-help] ...\n");
        return 0;
    }

    // Create the VM with specified architecture, amount of RAM / cores
    rvvm_machine_t* machine = rvvm_create_machine(mem, smp, isa);
    if (machine == NULL) {
        rvvm_error("Failed to create VM");
        return -1;
    }

    // Initialize basic peripherals (Interrupt controllers, PCI bus, clocks, power management)
    riscv_clint_init_auto(machine);

    if (rvvm_has_arg("riscv_aia")) {
        // Use RISC-V Advanced Interrupt Architecture (IMSIC + APLIC)
        riscv_imsic_init_auto(machine);
        riscv_aplic_init_auto(machine);
    } else {
        // Use SiFive PLIC
        riscv_plic_init_auto(machine);
    }

    pci_bus_init_auto(machine);
    i2c_oc_init_auto(machine);

    // usb_xhci_init(rvvm_get_pci_bus(machine));

    rtc_goldfish_init_auto(machine);
    syscon_init_auto(machine);

    if (rvvm_has_arg("gdbstub")) {
        gdbstub_init(machine, rvvm_getarg("gdbstub"));
    }

    if (!rvvm_has_arg("serial")) {
        ns16550a_init_term_auto(machine);
    }

    if (!rvvm_has_arg("nogui") && !rvvm_has_arg("res")) {
        if (rvvm_has_arg("bochs_display")) {
            gui_window_t* win = gui_rvvm_init(RVVM_BOCHS_DISPLAY_VRAM, NULL, machine);
            rvvm_bochs_display_init_auto(machine, gui_window_get_fbdev(win));
        } else {
            gui_window_t* win = gui_rvvm_init(0, NULL, machine);
            rvvm_simplefb_init_auto(machine, gui_window_get_fbdev(win));
        }
    }

    if (rvvm_has_arg("hda_test")) {
        sound_hda_init_auto(machine);
    }

    tap_dev_t* tap = NULL;
#ifdef USE_NET
    if (!rvvm_has_arg("nonet")) {
        tap = tap_open();
        rtl8169_init(rvvm_get_pci_bus(machine), tap);
    }
#endif

    if (!rvvm_cli_configure(machine, bios, tap)) {
        rvvm_error("Failed to initialize VM");
        rvvm_free_machine(machine);
        return -1;
    }

    if (rvvm_getarg("cmdline")) {
        rvvm_set_cmdline(machine, rvvm_getarg("cmdline"));
    }
    if (rvvm_getarg("append")) {
        rvvm_append_cmdline(machine, rvvm_getarg("append"));
    }
    if (rvvm_getarg("dumpdtb")) {
        // Dump the machine DTB after it's been fully configured
        rvvm_dump_fdt(machine, rvvm_getarg("dumpdtb"));
    }

    if (!rvvm_has_arg("noisolation")) {
        // Preparations are done, isolate the process as much as possible
        rvvm_restrict_process();
    }

    rvvm_start_machine(machine);

    // Returns on machine shutdown
    rvvm_run_eventloop();

    rvvm_free_machine(machine);
    return 0;
}

int main(int argc, char** argv)
{
#if defined(HOST_TARGET_WINNT)
    // Prefer UTF-8 console on Windows
    SetConsoleOutputCP(CP_UTF8);
    SetConsoleCP(CP_UTF8);

    // Prefer UTF-8 arguments on Windows
    fill_argv_envp(&argc, &argv, NULL);
#endif
    return rvvm_cli_main(argc, argv);
}

POP_OPTIMIZATION_SIZE
