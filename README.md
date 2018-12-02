# kcmask for Centos 7.5
## Installation

`make menuconfig` // remember to select the kcmask

`make -j4` //if you have 4 cpus

`make -j4 modules`

`make install`

`make modules_install`

## Check memory reservation

vi /etc/default/grub and look at the last line.

## Remeber to remove printk when you finish debugging
# kcmask for Ubuntu 16.04
## Reserve memory and Update grub (memory size should be larger than 4GB. Recommend 10GB)
`sudo vim /etc/default/grub`

replace `GRUB_CMDLINE_LINUX_DEFAULT=""` with `GRUB_CMDLINE_LINUX_DEFAULT="quiet splash memmap=8K\\\$0x100000000"`

`sudo update-grub`

`reboot`

Then, do `dmsg|grep mem`

You can find the following two lines, which means you successfully reserved 8KB memory starting from 4GB physical address. 

`[    0.000000] user: [mem 0x00000000********-0x0000000100001fff] reserved`

`[    0.000000] user: [mem 0x0000000100002000-0x0000000*********] usable`

## Compile and install modified kernel. Note that kernel 4.4.10 is for Ubuntu16.04 and kernel 3.10.0 is for Centos 7.5

`make menuconfig`

Then check and select KCMASK module in `proccessor type and features-->`

`make -j4 deb-pkg` if you have 4 cores. Be sure to assign 4 or more cores to your virtual machine.

`dpkg -i linux-*.deb` to install all deb packages in `cd ..` upper level directory

`reboot` and select the kernel verison `ubuntu, with linux 4.4.1012-2`
