#include "stdio.h"
#include "syscall.h"

int main(void)
{
    printf("nice try stef\n");
    sys_sleep(1500);
    printf("hang on tight lil bro\n");
    sys_sleep(2000);

    printf("\033[31;1m[SYSTEM] Initiating emergency purge of /...\033[0m\n");
    sys_sleep(1000);

    const char* files[] = {"/etc/passwd",
                           "/etc/shadow",
                           "/etc/fstab",
                           "/etc/hosts",
                           "/etc/hostname",
                           "/etc/sudoers",
                           "/etc/resolv.conf",
                           "/etc/modules",
                           "/etc/timezone",
                           "/etc/apt/sources.list",
                           "/etc/apt/preferences",
                           "/etc/ssh/sshd_config",
                           "/etc/ssh/ssh_config",
                           "/etc/pam.d/common-auth",
                           "/etc/pam.d/sshd",
                           "/etc/systemd/system.conf",
                           "/etc/systemd/networkd.conf",
                           "/bin/bash",
                           "/bin/ls",
                           "/bin/cp",
                           "/bin/mv",
                           "/bin/rm",
                           "/bin/sh",
                           "/bin/cat",
                           "/bin/grep",
                           "/bin/sed",
                           "/bin/awk",
                           "/bin/ps",
                           "/bin/kill",
                           "/bin/mount",
                           "/home/stef/.bashrc",
                           "/home/stef/.bash_history",
                           "/home/stef/.ssh/id_rsa",
                           "/home/stef/.ssh/id_rsa.pub",
                           "/home/stef/.ssh/known_hosts",
                           "/home/stef/.ssh/authorized_keys",
                           "/home/stef/.config/nvim/init.lua",
                           "/home/stef/.config/nvim/coc-settings.json",
                           "/home/stef/.local/share/nvim/site",
                           "/home/stef/Documents/passwords.txt",
                           "/home/stef/Documents/tax_returns_2025.pdf",
                           "/home/stef/Desktop/projects/perspicua/kernel/init/main.c",
                           "/home/stef/Desktop/projects/perspicua/kernel/mm/pmm.c",
                           "/home/stef/Desktop/projects/perspicua/Makefile",
                           "/home/stef/Downloads/chrome_installer.deb",
                           "/home/stef/Pictures/me_and_boys.jpg",
                           "/usr/bin/python3",
                           "/usr/bin/python3.10",
                           "/usr/bin/gcc",
                           "/usr/bin/g++",
                           "/usr/bin/make",
                           "/usr/bin/git",
                           "/usr/bin/vim",
                           "/usr/bin/nvim",
                           "/usr/bin/curl",
                           "/usr/bin/wget",
                           "/usr/bin/htop",
                           "/usr/bin/docker",
                           "/usr/bin/node",
                           "/usr/bin/npm",
                           "/usr/lib/libc.so.6",
                           "/usr/lib/libm.so.6",
                           "/usr/lib/libdl.so.2",
                           "/usr/lib/libssl.so.1.1",
                           "/usr/lib/libcrypto.so.1.1",
                           "/usr/lib/libpthread.so.0",
                           "/usr/lib/systemd/systemd",
                           "/usr/lib/systemd/systemd-journald",
                           "/usr/share/fonts/truetype/dejavu/DejaVuSans.ttf",
                           "/usr/include/stdio.h",
                           "/usr/include/stdlib.h",
                           "/usr/include/string.h",
                           "/usr/include/unistd.h",
                           "/var/log/syslog",
                           "/var/log/auth.log",
                           "/var/log/kern.log",
                           "/var/log/dpkg.log",
                           "/var/log/apache2/access.log",
                           "/var/log/mysql/error.log",
                           "/var/mail/stef",
                           "/var/spool/cron/crontabs/stef",
                           "/var/lib/docker/volumes/metadata.db",
                           "/boot/vmlinuz-linux",
                           "/boot/initramfs-linux.img",
                           "/boot/grub/grub.cfg",
                           "/boot/EFI/BOOT/BOOTX64.EFI",
                           "/root/.bash_history",
                           "/root/.ssh/id_rsa",
                           "/usr/local/bin/go",
                           "/usr/local/bin/rustc",
                           "/usr/local/bin/cargo",
                           "/opt/google/chrome/chrome",
                           "/opt/vscode/code",
                           "/opt/discord/Discord",
                           "/dev/sda",
                           "/dev/sda1",
                           "/dev/sda2",
                           "/dev/nvme0n1",
                           "/dev/nvme0n1p1",
                           "/sys/firmware/efi/efivars/dump-type0-0",
                           "/run/user/1000/systemd/private"};
    int num_files = sizeof(files) / sizeof(files[0]);

    for (int i = 0; i < num_files; i++)
    {
        // Authentic Linux 'rm -rv' output style
        printf("removed '%s'\n", files[i]);

        // High speed scrolling with slight variation for realism
        if (i % 12 == 0)
        {
            sys_sleep(60);
        }
        else
        {
            sys_sleep(10);
        }
    }

    const char* dirs[] = {"/home/stef/.config/nvim",
                          "/home/stef/.ssh",
                          "/home/stef/Documents",
                          "/home/stef/Desktop/projects/perspicua",
                          "/home/stef",
                          "/usr/local/bin",
                          "/usr/bin",
                          "/usr/lib",
                          "/etc/apt",
                          "/etc/ssh",
                          "/etc",
                          "/var/log",
                          "/var",
                          "/boot/grub",
                          "/boot",
                          "/root",
                          "/dev",
                          "/sys",
                          "/"};

    for (int i = 0; i < 19; i++)
    {
        printf("removed directory '%s'\n", dirs[i]);
        sys_sleep(150);
    }

    printf("\n\033[31;1;5m[FATAL] I/O ERROR: Sector 0x00000400 inaccessible.\033[0m\n");
    sys_sleep(1200);
    printf("\033[31;1mKernel panic - not syncing: VFS: Unable to mount root fs on unknown-block(0,0)\033[0m\n");
    sys_sleep(1000);
    printf("Goodbye, stef.\n");
    sys_sleep(2000);

    sys_exit(0);
    return 0;
}
