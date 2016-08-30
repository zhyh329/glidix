.PHONY: all
all: out/cp out/lspci out/clear-screen out/dhcp out/resolve out/route out/sudo out/mip-install out/passwd out/mount out/gxpad out/mkdir out/sigsegv out/ping out/ls out/sleep out/date out/rmmod out/login out/linkslaac out/color out/chgrp out/ping6 out/chown out/srv-wrapper out/insmod out/whoami out/whois out/env out/halt out/service out/logmgr out/wget out/clock out/stat out/pwdsetup out/sniff out/rm out/umount out/kill out/crypt out/mkmip out/chmod out/touch out/cat out/gfxterm out/ln out/netconf out/eject
out/cp: src/cp.c
	x86_64-glidix-gcc $< -o $@ 
out/lspci: src/lspci.c
	x86_64-glidix-gcc $< -o $@ 
out/clear-screen: src/clear-screen.c
	x86_64-glidix-gcc $< -o $@ 
out/dhcp: src/dhcp.c
	x86_64-glidix-gcc $< -o $@ 
out/resolve: src/resolve.c
	x86_64-glidix-gcc $< -o $@ 
out/route: src/route.c
	x86_64-glidix-gcc $< -o $@ 
out/sudo: src/sudo.c
	x86_64-glidix-gcc $< -o $@ -lcrypt
	chmod 6755 $@
out/mip-install: src/mip-install.c
	x86_64-glidix-gcc $< -o $@ 
out/passwd: src/passwd.c
	x86_64-glidix-gcc $< -o $@ -lcrypt
	chmod 6755 $@
out/mount: src/mount.c
	x86_64-glidix-gcc $< -o $@ 
out/gxpad: src/gxpad.c
	x86_64-glidix-gcc $< -o $@ 
out/mkdir: src/mkdir.c
	x86_64-glidix-gcc $< -o $@ 
out/sigsegv: src/sigsegv.c
	x86_64-glidix-gcc $< -o $@ 
out/ping: src/ping.c
	x86_64-glidix-gcc $< -o $@ 
	chmod 6755 $@
out/ls: src/ls.c
	x86_64-glidix-gcc $< -o $@ 
out/sleep: src/sleep.c
	x86_64-glidix-gcc $< -o $@ 
out/date: src/date.c
	x86_64-glidix-gcc $< -o $@ 
out/rmmod: src/rmmod.c
	x86_64-glidix-gcc $< -o $@ 
out/login: src/login.c
	x86_64-glidix-gcc $< -o $@ -lcrypt
out/linkslaac: src/linkslaac.c
	x86_64-glidix-gcc $< -o $@ 
out/color: src/color.c
	x86_64-glidix-gcc $< -o $@ 
out/chgrp: src/chgrp.c
	x86_64-glidix-gcc $< -o $@ 
out/ping6: src/ping6.c
	x86_64-glidix-gcc $< -o $@ 
	chmod 6755 $@
out/chown: src/chown.c
	x86_64-glidix-gcc $< -o $@ 
out/srv-wrapper: src/srv-wrapper.c
	x86_64-glidix-gcc $< -o $@ 
out/insmod: src/insmod.c
	x86_64-glidix-gcc $< -o $@ 
out/whoami: src/whoami.c
	x86_64-glidix-gcc $< -o $@ 
out/whois: src/whois.c
	x86_64-glidix-gcc $< -o $@ 
out/env: src/env.c
	x86_64-glidix-gcc $< -o $@ 
out/halt: src/halt.c
	x86_64-glidix-gcc $< -o $@ 
out/service: src/service.c
	x86_64-glidix-gcc $< -o $@ 
out/logmgr: src/logmgr.c
	x86_64-glidix-gcc $< -o $@ 
out/wget: src/wget.c
	x86_64-glidix-gcc $< -o $@ 
out/clock: src/clock.c
	x86_64-glidix-gcc $< -o $@ 
out/stat: src/stat.c
	x86_64-glidix-gcc $< -o $@ 
out/pwdsetup: src/pwdsetup.c
	x86_64-glidix-gcc $< -o $@ -lcrypt
out/sniff: src/sniff.c
	x86_64-glidix-gcc $< -o $@ 
out/rm: src/rm.c
	x86_64-glidix-gcc $< -o $@ 
out/umount: src/umount.c
	x86_64-glidix-gcc $< -o $@ 
out/kill: src/kill.c
	x86_64-glidix-gcc $< -o $@ 
out/crypt: src/crypt.c
	x86_64-glidix-gcc $< -o $@ -lcrypt
out/mkmip: src/mkmip.c
	x86_64-glidix-gcc $< -o $@ 
out/chmod: src/chmod.c
	x86_64-glidix-gcc $< -o $@ 
out/touch: src/touch.c
	x86_64-glidix-gcc $< -o $@ 
out/cat: src/cat.c
	x86_64-glidix-gcc $< -o $@ 
out/gfxterm: src/gfxterm.c
	x86_64-glidix-gcc $< -o $@ 
out/ln: src/ln.c
	x86_64-glidix-gcc $< -o $@ 
out/netconf: src/netconf.c
	x86_64-glidix-gcc $< -o $@ 
out/eject: src/eject.c
	x86_64-glidix-gcc $< -o $@ 
