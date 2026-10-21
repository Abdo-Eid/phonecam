"""Scoped, DPAPI-encrypted backup/reset for PhoneCam's live-device tests.

Never touches another app, Windows credentials outside PhoneCam/*, camera
registration, or USB drivers. Requires the installed debuggable Android APK.
"""
import argparse
import base64
import ctypes as c
from ctypes import wintypes as w
import hashlib
import json
from pathlib import Path
import subprocess
import winreg

PACKAGE = "io.github.abdoeid.phonecam"
SETTINGS = r"Software\PhoneCam\Connection"


class Credential(c.Structure):
    _fields_ = [("flags", w.DWORD), ("type", w.DWORD), ("target", w.LPWSTR),
                ("comment", w.LPWSTR), ("written", w.FILETIME), ("size", w.DWORD),
                ("blob", c.POINTER(c.c_ubyte)), ("persist", w.DWORD),
                ("attributes_count", w.DWORD), ("attributes", c.c_void_p),
                ("alias", w.LPWSTR), ("user", w.LPWSTR)]


class Blob(c.Structure):
    _fields_ = [("size", w.DWORD), ("data", c.POINTER(c.c_ubyte))]


advapi = c.WinDLL("advapi32", use_last_error=True)
crypt = c.WinDLL("crypt32", use_last_error=True)
kernel = c.WinDLL("kernel32", use_last_error=True)
advapi.CredEnumerateW.argtypes = [w.LPCWSTR, w.DWORD, c.POINTER(w.DWORD), c.POINTER(c.POINTER(c.POINTER(Credential)))]
advapi.CredEnumerateW.restype = w.BOOL
advapi.CredFree.argtypes = [c.c_void_p]
advapi.CredDeleteW.argtypes = [w.LPCWSTR, w.DWORD, w.DWORD]
advapi.CredWriteW.argtypes = [c.POINTER(Credential), w.DWORD]
crypt.CryptProtectData.argtypes = [c.POINTER(Blob), w.LPCWSTR, c.c_void_p, c.c_void_p, c.c_void_p, w.DWORD, c.POINTER(Blob)]
crypt.CryptUnprotectData.argtypes = [c.POINTER(Blob), c.c_void_p, c.c_void_p, c.c_void_p, c.c_void_p, w.DWORD, c.POINTER(Blob)]
kernel.LocalFree.argtypes = [c.c_void_p]


def protect(data, decrypt=False):
    storage = (c.c_ubyte * len(data)).from_buffer_copy(data)
    source, dest = Blob(len(data), storage), Blob()
    fn = crypt.CryptUnprotectData if decrypt else crypt.CryptProtectData
    label = None if decrypt else "PhoneCam test backup"
    if not fn(c.byref(source), label, None, None, None, 1, c.byref(dest)):
        raise c.WinError(c.get_last_error())
    try:
        return c.string_at(dest.data, dest.size)
    finally:
        kernel.LocalFree(dest.data)


def credentials():
    count = w.DWORD()
    items = c.POINTER(c.POINTER(Credential))()
    if not advapi.CredEnumerateW("PhoneCam/*", 0, c.byref(count), c.byref(items)):
        if c.get_last_error() == 1168:
            return []
        raise c.WinError(c.get_last_error())
    try:
        return [dict(target=x.target, type=x.type, persist=x.persist,
                     user=x.user, comment=x.comment, alias=x.alias,
                     blob=base64.b64encode(c.string_at(x.blob, x.size)).decode())
                for x in (items[i].contents for i in range(count.value))]
    finally:
        advapi.CredFree(items)


def registry():
    try:
        with winreg.OpenKey(winreg.HKEY_CURRENT_USER, SETTINGS) as key:
            return [winreg.EnumValue(key, i) for i in range(winreg.QueryInfoKey(key)[1])]
    except FileNotFoundError:
        return None


def clear_windows():
    for item in credentials():
        if not advapi.CredDeleteW(item["target"], item["type"], 0):
            raise c.WinError(c.get_last_error())
    try:
        winreg.DeleteKey(winreg.HKEY_CURRENT_USER, SETTINGS)
    except FileNotFoundError:
        pass


def adb(serial, *args, data=None):
    result = subprocess.run(["adb", "-s", serial, *args], input=data, capture_output=True, timeout=30)
    if result.returncode:
        raise RuntimeError(f"adb operation failed: {args[:3]}: {result.stderr.decode(errors='replace').strip()}")
    return result.stdout


def stop_phone(serial):
    adb(serial, "shell", "am", "force-stop", PACKAGE)


def clear_phone_data(serial):
    result = subprocess.run(["adb", "-s", serial, "shell", "pm", "clear", PACKAGE],
                            capture_output=True, timeout=30)
    if result.returncode == 0 and b"Success" in result.stdout:
        return
    if b"SecurityException" not in result.stderr + result.stdout:
        raise RuntimeError("Phone data reset failed")
    # MIUI may deny CLEAR_APP_USER_DATA to adb. An app can clear its own data
    # using the public ActivityManager API; the dedicated test is opt-in only.
    subprocess.run(["adb", "-s", serial, "shell", "am", "instrument", "-w", "-e", "class",
                    "io.github.abdoeid.phonecam.DataResetTest", "-e", "allowDataReset", "true",
                    PACKAGE + ".test/androidx.test.runner.AndroidJUnitRunner"],
                   capture_output=True, timeout=30)
    # The reset kills instrumentation, so its exit status isn't proof. Verify
    # the actual app-owned storage is empty (apart from newly created caches).
    listing = adb(serial, "shell", "run-as", PACKAGE, "ls").decode().split()
    if any(name in listing for name in ("shared_prefs", "files", "databases", "no_backup")):
        raise RuntimeError("MIUI self-reset did not clear private data; use Android Settings manually")


def read_backup(path):
    state = json.loads(protect(path.read_bytes(), decrypt=True))
    if state.get("package") != PACKAGE or state.get("version") != 1:
        raise ValueError("Not a PhoneCam test backup")
    return state


def backup(serial, path):
    if path.exists():
        raise FileExistsError("Refusing to overwrite an existing backup")
    stop_phone(serial)
    listing = adb(serial, "shell", "run-as", PACKAGE, "ls").decode().split()
    dirs = [d for d in ("shared_prefs", "files", "databases", "no_backup") if d in listing]
    archive = adb(serial, "exec-out", "run-as", PACKAGE, "tar", "-cf", "-", *dirs) if dirs else b""
    package = adb(serial, "shell", "dumpsys", "package", PACKAGE).decode()
    permissions = [p for p in ("android.permission.CAMERA", "android.permission.POST_NOTIFICATIONS")
                   if f"{p}: granted=true" in package]
    state = dict(version=1, package=PACKAGE, serial=serial, phone=base64.b64encode(archive).decode(),
                 permissions=permissions, registry=registry(), credentials=credentials())
    encoded = json.dumps(state, sort_keys=True).encode()
    path.parent.mkdir(parents=True, exist_ok=True)
    path.write_bytes(protect(encoded))
    if protect(path.read_bytes(), decrypt=True) != encoded:
        raise RuntimeError("Backup verification failed")
    print("PASS encrypted phone/PC backup verified (no credentials printed)")


def restore(serial, path, manual_permissions=False):
    state = read_backup(path)
    if state["serial"] != serial:
        raise ValueError("Backup belongs to another phone")
    stop_phone(serial)
    # Replace only the app-owned data directories. Unlike another full package
    # reset, this does not revoke camera permission the user just granted.
    adb(serial, "shell", "run-as", PACKAGE, "rm", "-rf", "shared_prefs", "files", "databases", "no_backup")
    archive = base64.b64decode(state["phone"])
    if archive:
        adb(serial, "exec-in", "run-as", PACKAGE, "tar", "-xf", "-", data=archive)
    for permission in state["permissions"]:
        package = adb(serial, "shell", "dumpsys", "package", PACKAGE).decode()
        if f"{permission}: granted=true" not in package:
            if manual_permissions:
                print("Camera permission needs a manual Allow after reopening")
            else:
                adb(serial, "shell", "pm", "grant", PACKAGE, permission)
    clear_windows()
    if state["registry"] is not None:
        with winreg.CreateKey(winreg.HKEY_CURRENT_USER, SETTINGS) as key:
            for name, value, kind in state["registry"]:
                winreg.SetValueEx(key, name, 0, kind, value)
    for item in state["credentials"]:
        payload = base64.b64decode(item["blob"])
        storage = (c.c_ubyte * len(payload)).from_buffer_copy(payload)
        record = Credential()
        record.type, record.target, record.persist = item["type"], item["target"], item["persist"]
        record.user, record.comment, record.alias = item["user"], item["comment"], item["alias"]
        record.size, record.blob = len(payload), storage
        if not advapi.CredWriteW(c.byref(record), 0):
            raise c.WinError(c.get_last_error())
    if sorted(credentials(), key=lambda x: x["target"]) != sorted(state["credentials"], key=lambda x: x["target"]):
        raise RuntimeError("Credential restoration verification failed")
    if sorted(registry() or []) != sorted(tuple(value) for value in (state["registry"] or [])):
        raise RuntimeError("Settings restoration verification failed")
    if archive:
        # Tar metadata need not be identical; compare all archived file contents.
        import io
        import tarfile
        with tarfile.open(fileobj=io.BytesIO(archive)) as tar:
            for member in tar.getmembers():
                if member.isfile():
                    expected = hashlib.sha256(tar.extractfile(member).read()).digest()
                    actual = hashlib.sha256(adb(serial, "exec-out", "run-as", PACKAGE, "cat", member.name)).digest()
                    if expected != actual:
                        raise RuntimeError("Phone file restoration verification failed")
    print("PASS original phone data, pairing, Windows credentials and settings restored and verified")


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("action", choices=("backup", "restore", "clear-data", "clear-cache"))
    parser.add_argument("--serial", required=True)
    parser.add_argument("--backup", type=Path, required=True)
    parser.add_argument("--allow-data-reset", action="store_true")
    parser.add_argument("--manual-permissions", action="store_true")
    args = parser.parse_args()
    if args.action == "backup":
        backup(args.serial, args.backup)
    elif args.action == "restore":
        restore(args.serial, args.backup, args.manual_permissions)
    else:
        state = read_backup(args.backup)
        if state["serial"] != args.serial:
            raise ValueError("Backup belongs to another phone")
        stop_phone(args.serial)
        if args.action == "clear-data":
            if not args.allow_data_reset:
                raise ValueError("Data reset requires --allow-data-reset and a verified backup")
            clear_phone_data(args.serial)
            clear_windows()
            print("PASS PhoneCam-only phone data, Windows connection settings and credentials cleared")
        else:
            adb(args.serial, "shell", "run-as", PACKAGE, "rm", "-rf", "cache", "code_cache")
            print("PASS phone cache cleared; native desktop has no separate app cache (pairing retained)")


if __name__ == "__main__":
    main()
