#!/usr/bin/env python3
# Test double for "cryptsetup" and "dmsetup" on kernels without device-mapper.
#
# An "open" LUKS volume is a loop device over the partition starting at the LUKS data
# offset, without any encryption (identity mapping): file system tools then work on
# /dev/mapper/<name> as usual.  Passphrases, the LUKS header and luksFormat are real
# (handled by the real cryptsetup).  Used only by the QtParted tests via PATH.
import fcntl, json, os, re, struct, subprocess, sys

STATE = os.environ.get("FAKECRYPT_STATE", "/run/qtparted-fakecrypt")
REAL = "/usr/sbin/cryptsetup"
LOOP_GET_STATUS64, LOOP_SET_STATUS64, LOOP_SET_CAPACITY = 0x4C05, 0x4C04, 0x4C07
STATUS64 = "QQQQQIIII64s64s32s2Q"  # struct loop_info64


def state_path(name):
    return os.path.join(STATE, name + ".json")


def load(name):
    with open(state_path(name)) as f:
        return json.load(f)


def all_states():
    if not os.path.isdir(STATE):
        return []
    return [load(n[:-5]) for n in sorted(os.listdir(STATE)) if n.endswith(".json")]


def dev_bytes(path):
    return int(subprocess.check_output(["blockdev", "--getsize64", path]).decode())


def data_offset(dev):
    out = subprocess.check_output([REAL, "luksDump", dev]).decode()
    seg = out.split("Data segments:", 1)[1]
    return int(re.search(r"offset:\s*(\d+)", seg).group(1))


def set_sizelimit(loop, limit):
    with open(loop, "rb") as f:
        buf = bytearray(struct.calcsize(STATUS64))
        fcntl.ioctl(f, LOOP_GET_STATUS64, buf)
        fields = list(struct.unpack(STATUS64, bytes(buf)))
        fields[4] = limit  # lo_sizelimit (after lo_device, lo_inode, lo_rdevice, lo_offset)
        fcntl.ioctl(f, LOOP_SET_STATUS64, struct.pack(STATUS64, *fields))
        fcntl.ioctl(f, LOOP_SET_CAPACITY, 0)


def test_passphrase(dev, pw):
    return subprocess.run([REAL, "luksOpen", "--test-passphrase", dev], input=pw).returncode == 0


def cryptsetup(args):
    words = []
    skip = False
    for a in args:
        if skip:
            skip = False
        elif a == "--size":
            skip = True
        elif not a.startswith("-"):
            words.append(a)
    if not words or words[0] not in ("luksOpen", "open", "luksClose", "close", "resize"):
        os.execv(REAL, [REAL] + args)
    cmd = words[0]
    # Passphrases come on stdin (closed by the caller) for open and resize only.
    pw = b"" if cmd in ("luksClose", "close") or sys.stdin.isatty() else sys.stdin.buffer.read()
    if cmd in ("luksOpen", "open"):
        dev, name = words[1], words[2] if len(words) > 2 else None
        if not test_passphrase(dev, pw):
            print("No key available with this passphrase.", file=sys.stderr)
            return 2
        if "--test-passphrase" in args:
            return 0
        off = data_offset(dev)
        size = dev_bytes(dev) - off
        loop = subprocess.check_output(["losetup", "-f", "--show", "-o", str(off), "--sizelimit", str(size),
                                        dev]).decode().strip()
        os.makedirs(STATE, exist_ok=True)
        os.makedirs("/dev/mapper", exist_ok=True)
        link = "/dev/mapper/" + name
        if os.path.lexists(link):
            os.unlink(link)
        os.symlink(loop, link)
        with open(state_path(name), "w") as f:
            json.dump({"name": name, "dev": dev, "loop": loop, "offset": off, "uuid": os.path.basename(dev)}, f)
        return 0
    if cmd in ("luksClose", "close"):
        name = words[1]
        st = load(name)
        r = subprocess.run(["losetup", "-d", st["loop"]], capture_output=True)
        if r.returncode != 0:
            sys.stderr.write(r.stderr.decode())
            return 5
        os.unlink("/dev/mapper/" + name)
        os.unlink(state_path(name))
        return 0
    if cmd == "resize":
        name = words[1]
        st = load(name)
        # The volume key is "in the kernel keyring": a resize needs the passphrase.
        if not test_passphrase(st["dev"], pw):
            print("No key available with this passphrase.", file=sys.stderr)
            return 2
        if "--size" in args:
            size = int(args[args.index("--size") + 1]) * 512
        else:
            size = dev_bytes(st["dev"]) - st["offset"]
        set_sizelimit(st["loop"], size)
        print("Command successful.")
        return 0
    return 1


def dmsetup(args):
    if args[:3] != ["table", "--target", "crypt"]:
        print("fake dmsetup: unsupported", args, file=sys.stderr)
        return 1
    states = all_states()
    if not states:
        print("No devices found")
        return 0
    for st in states:
        rdev = os.stat(st["dev"]).st_rdev
        length = dev_bytes(st["loop"]) // 512
        print("%s: 0 %d crypt aes-xts-plain64 :64:logon:cryptsetup:fake-d0 0 %d:%d %d"
              % (st["name"], length, os.major(rdev), os.minor(rdev), st["offset"] // 512))
    return 0


if __name__ == "__main__":
    tool = os.path.basename(sys.argv[0])
    sys.exit(dmsetup(sys.argv[1:]) if tool == "dmsetup" else cryptsetup(sys.argv[1:]))
