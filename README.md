# esp-azipod
Azipod Build for Virtual Sailor NG

## Setup


Create the following udev rule:

```bash
/etc/udev/rules.d/91-uinput.rules
```

```bash
KERNEL=="uinput", SUBSYSTEM=="misc", GROUP="input", MODE:="0660", OPTIONS+="static_node=uinput"
```

Add your user to the `input` group`

```bash
sudo usermod -aG input $USER
```

Reboot.

Check the permissions on `/dev/uinput`.

```bash
 $ ls -al /dev/uinput

crw-rw---- 1 root input 10, 223 Oct 27 11:58 /dev/uinput
```
