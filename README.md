# kcmask
## Installation

`make menuconfig` // remember to select the kcmask

`make -j4` //if you have 4 cpus

`make -j4 modules`

`make install`

`make modules_install`

## Check memory reservation

vi /etc/default/grub and look at the last line.

## Remeber to remove printk when you finish debugging
