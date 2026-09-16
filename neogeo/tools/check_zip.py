#!/usr/bin/env python3
"""Verify a game zip against the gngeo drv (names + CRCs).
Usage: python3 check_zip.py <game>.zip [gngeo_data.zip]
Exit 0 = all required ROMs verified, cache generation will work.
"""
import sys, os, zipfile, zlib

def main():
    if len(sys.argv) < 2:
        print(__doc__); sys.exit(2)
    zip_path = sys.argv[1]
    datafile = sys.argv[2] if len(sys.argv) > 2 else os.path.join(
        os.path.dirname(os.path.abspath(__file__)), 'SDcard', 'roms', 'neogeo', 'gngeo_data.zip')
    if not os.path.exists(datafile):
        datafile = os.path.join(os.path.dirname(os.path.abspath(zip_path)), 'gngeo_data.zip')
    game = os.path.splitext(os.path.basename(zip_path))[0]
    # 常见后缀变体回退: xxx_auth / _fixed / (1) 等 → 去掉修饰词
    for suf in ('_auth', '_fixed', '_new'):
        if game.endswith(suf):
            game = game[: -len(suf)]
            break

    dzf = zipfile.ZipFile(datafile)
    drv = dzf.read(f'rom/{game}.drv')
    off = 240
    nb = int.from_bytes(drv[236:240], 'little')
    roms = []
    for _ in range(nb):
        fn = drv[off:off+32].split(b'\0')[0].decode('ascii', 'replace'); off += 32
        region = drv[off]; off += 1
        src = int.from_bytes(drv[off:off+4], 'little'); off += 4
        dest = int.from_bytes(drv[off:off+4], 'little'); off += 4
        size = int.from_bytes(drv[off:off+4], 'little'); off += 4
        crc = int.from_bytes(drv[off:off+4], 'little'); off += 4
        if crc and size:
            roms.append((fn, size, crc))
    parent = None
    try:
        pd = dzf.read(f'rom/{game}.drv')  # parent handled via gngeo_data only
    except KeyError:
        pass

    zf = zipfile.ZipFile(zip_path)
    members = {}
    for n in zf.namelist():
        members.setdefault(os.path.splitext(n.lower())[0], n)

    ok = True
    # Build CRC → member index (CRC is the true identifier, names don't matter)
    crc_map = {}
    for n in zf.namelist():
        data = zf.read(n)
        c = zlib.crc32(data) & 0xffffffff
        crc_map[c] = (n, data, len(data))

    for fn, size, crc in roms:
        if base.startswith('sfix') if (base := os.path.splitext(fn.lower())[0]) else False:
            print(f"  OPTIONAL {fn:22s} (BIOS 公共文件)")
            continue
        if crc in crc_map:
            n, data, dlen = crc_map[crc]
            print(f"  OK       {fn:22s} (zip内: {os.path.basename(n)})")
        elif crc in crc_map or any(crc == (zlib.crc32(zf.read(n)) & 0xffffffff) for n in zf.namelist() if os.path.splitext(n.lower())[0] == base):
            pass  # already handled above
        else:
            # try to find by CRC in the whole zip
            found = False
            for n in zf.namelist():
                data = zf.read(n)
                if (zlib.crc32(data) & 0xffffffff) == crc:
                    print(f"  OK*      {fn:22s} (CRC匹配: {os.path.basename(n)})")
                    found = True
                    break
            if not found:
                print(f"  MISSING  {fn:22s} (需要 CRC={crc:08X} size={size})")
                ok = False

    print(("=" * 40))
    if ok:
        print(f"✅ {game}: 全部 ROM 校验通过，可直接用 gen_ctile.py 生成缓存")
    else:
        print(f"❌ {game}: ROM 不匹配（不同切分/版本/未解密变体），请更换 zip")
    sys.exit(0 if ok else 1)

main()
