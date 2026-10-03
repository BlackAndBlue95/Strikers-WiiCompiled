# The Super Team's front-end art in Charged's style, from SMS's captain-select head tile
# (choose_capt_super): captain select's portraits (lit and greyed: the head on a warm backdrop like
# Charged's captain portraits) and partner select's board heads (cut out, dark outline, both facings).
import os, math, sys
sys.path.insert(0, os.path.join(os.path.dirname(os.path.abspath(__file__)), '..'))
import gxtex as gx  # noqa: E402 (scripts/modkit)

def robot_head(sms):
    """SMS's tile cut down to the robot: no band behind it, no specks; cropped to it."""
    head = gx.decode(sms[gx.lower_hash('fe/mainv2/targas/choose_capt_super')])
    head = [[p if p[3] >= 100 else (0, 0, 0, 0) for p in row] for row in head]
    H, W = len(head), len(head[0])
    clear = lambda y, x: not (0 <= y < H and 0 <= x < W) or head[y][x][3] == 0
    # A dark-blue band runs behind the head: flood it away from its outer edge, through the band's
    # tones only, so the helmet's own dark blues stay.
    tones = [(0x29, 0x41, 0x52), (0x31, 0x41, 0x62), (0x31, 0x52, 0x62), (0x29, 0x52, 0x52)]
    band = lambda p: p[3] and min(sum((p[k] - t[k]) ** 2 for k in range(3)) for t in tones) <= 18 ** 2
    todo = [(y, x) for y in range(H) for x in range(W) if band(head[y][x]) and
            any(clear(y + dy, x + dx) for dy, dx in ((1, 0), (-1, 0), (0, 1), (0, -1)))]
    seen = set(todo)
    while todo:
        y, x = todo.pop()
        head[y][x] = (0, 0, 0, 0)
        for ny, nx in ((y + 1, x), (y - 1, x), (y, x + 1), (y, x - 1)):
            if 0 <= ny < H and 0 <= nx < W and (ny, nx) not in seen and band(head[ny][nx]):
                seen.add((ny, nx)); todo.append((ny, nx))
    for _ in range(2):  # the band's anti-aliased fringe: specks with few opaque neighbours
        keep = [[sum(1 for dy in (-1, 0, 1) for dx in (-1, 0, 1) if (dy or dx) and not clear(y + dy, x + dx)) >= 4
                 for x in range(W)] for y in range(H)]
        head = [[p if keep[y][x] else (0, 0, 0, 0) for x, p in enumerate(row)] for y, row in enumerate(head)]
    ys = [y for y, row in enumerate(head) if any(p[3] for p in row)]
    xs = [x for x in range(W) if any(head[y][x][3] for y in range(H))]
    return [row[min(xs):max(xs) + 1] for row in head[min(ys):max(ys) + 1]]

def fit(head, size):
    scale = size / max(len(head), len(head[0]))
    w, h = round(len(head[0]) * scale), round(len(head) * scale)
    return gx.resize(head, w, h), w, h

def portraits(head, msc):
    ref = msc[gx.lower_hash('fe/screens/images/captain_12_waluigi_s')]
    big, w, h = fit(head, 116)
    bg = []
    for y in range(128):
        row = []
        for x in range(128):
            d = math.hypot((x - 64) / 64, (y - 50) / 64)
            t = min(1.0, d / 1.2)
            r = int(250 * (1 - t) + 200 * t); g = int(200 * (1 - t) + 70 * t); b = int(40 * (1 - t) + 20 * t)
            v = max(0.55, 1 - 0.45 * max(0.0, d - 0.9))
            row.append((int(r * v), int(g * v), int(b * v), 255))
        bg.append(row)
    portrait = gx.over(bg, big, (128 - w) // 2 + 2, 128 - h)
    return {'captain_superteam_s': gx.encode_cmpr(portrait, ref),
            'captain_superteam_ds': gx.encode_cmpr(gx.grey(portrait), ref)}

def hud_icon(head, msc_icons):
    """The HUD's team icon (fe/captain_icons/captain_icons_<name>, CaptainIconsUI.res): like the
    captains', the face on the team's colour; steel blue for the robots."""
    ref = msc_icons[gx.lower_hash('fe/captain_icons/captain_icons_waluigi')]
    big, w, h = fit(head, 120)
    bg = [[(int(30 + 50 * (1 - y / 127)), int(55 + 60 * (1 - y / 127)), int(95 + 80 * (1 - y / 127)), 255)
           for x in range(128)] for y in range(128)]
    icon = gx.over(bg, big, (128 - w) // 2, 128 - h)
    return {'captain_icons_superteam': gx.encode_cmpr(icon, ref, mips=5)}

def loading_portrait(sms_loading, msc):
    """logos_TEAM_<name> (loading screen, in-game screens): head and shoulders on Charged's dark
    backdrop, from SMS's versus-screen pose (loadingscreens/mystery_l)."""
    ref = msc[gx.lower_hash('fe/screens/images/logos_TEAM_waluigi')]
    pose = gx.decode(sms_loading[gx.lower_hash('fe/loadingscreens/mystery_l')])
    crop = [row[96:256] for row in pose[0:160]]
    head = gx.resize(crop, 128, 128)
    bg = [[(int(70 - 55 * y / 127), int(70 - 55 * y / 127), int(74 - 56 * y / 127), 255) for x in range(128)] for y in range(128)]
    return {'logos_TEAM_superteam': gx.encode_cmpr(gx.over(bg, head), ref)}

def pda_portrait(sms_loading, msc):
    """attributes_<name> (captain select's stats panel, 128x256): head and torso on the team colour,
    from SMS's versus-screen pose."""
    ref = msc[gx.lower_hash('fe/screens/images/attributes_waluigi')]
    pose = gx.decode(sms_loading[gx.lower_hash('fe/loadingscreens/mystery_l')])
    crop = [row[118:246] for row in pose]  # 128 x 256: the head and chest
    bg = [[(int(40 + 40 * (1 - y / 255)), int(70 + 50 * (1 - y / 255)), int(120 + 70 * (1 - y / 255)), 255)
           for x in range(128)] for y in range(256)]
    return {'attributes_superteam': gx.encode_cmpr(gx.over(bg, crop), ref)}

def board_heads(head, msc):
    """<name>_right faces right; _left is its mirror (as Charged's own)."""
    ref = msc[gx.lower_hash('fe/screens/images/waluigi_right')]
    small, w, h = fit(head, 56)
    icon = gx.over([[(0, 0, 0, 0)] * 64 for _ in range(64)], small, (64 - w) // 2, (64 - h) // 2)
    solid = [[p[3] >= 128 for p in row] for row in icon]
    near = lambda y, x: any(0 <= y + dy < 64 and 0 <= x + dx < 64 and solid[y + dy][x + dx]
                            for dy in range(-2, 3) for dx in range(-2, 3) if dy * dy + dx * dx <= 5)
    right = [[p if solid[y][x] else (18, 16, 24, 255) if near(y, x) else (0, 0, 0, 0)
              for x, p in enumerate(row)] for y, row in enumerate(icon)]
    left = [list(reversed(row)) for row in right]
    return {'superteam_right': gx.encode_cmpr(right, ref), 'superteam_left': gx.encode_cmpr(left, ref)}

def build(sms_mainui, msc_fe_dir, catalogs, sms_loading=None):
    """Writes the textures into a mod's catalogs/fe folder."""
    sms = gx.fe_bundle(sms_mainui)
    loading = gx.fe_bundle(sms_loading or os.path.join(os.path.dirname(sms_mainui), 'LoadingScreensUI.Res'))
    msc = gx.fe_bundle(os.path.join(msc_fe_dir, 'mainui.dmn'))
    icons = gx.fe_bundle(os.path.join(msc_fe_dir, 'captainiconsui.res'))
    head = robot_head(sms)
    logo = loading_portrait(loading, msc)
    out = {('mainui.dmn', 'screens/images'): {**portraits(head, msc), **board_heads(head, msc), **logo,
                                               **pda_portrait(loading, msc)},
           ('gameloadingui.res', 'screens/images'): logo,
           ('ingameui.dmn', 'screens/images'): {**logo, **lowerthird(head, gx.fe_bundle(os.path.join(msc_fe_dir, 'ingameui.dmn')))},
           # The HUD icon: Charged's own, left on its disc from the Super Team's port.
           ('captainiconsui.res', 'captain_icons'): {'captain_icons_superteam': icons[gx.lower_hash('fe/captain_icons/captain_icons_super')]}}
    for (bundle, folder), textures in out.items():
        d = os.path.join(catalogs, 'fe', bundle, folder)
        os.makedirs(d, exist_ok=True)
        for name, blob in textures.items():
            open(os.path.join(d, name + '.gxt'), 'wb').write(blob)
    return {k: v for t in out.values() for k, v in t.items()}

def lowerthird(head, msc_ingame):
    """fe/screens/images/lowerthird_<name> (InGameUI: the goal and final-score banner's team emblem,
    128x64): the robot head, big and cropped as Charged's emblems are, on a light disc on the team's
    steel blue, inside the banners' dark frame (Yoshi's, which has no team colour in it)."""
    ref = msc_ingame[gx.lower_hash('fe/screens/images/lowerthird_yoshi')]
    tpl = gx.decode(ref)
    H, W = 64, 128
    clear = lambda y, x: not (0 <= y < H and 0 <= x < W) or tpl[y][x][3] < 128
    edge = [[min([abs(dy) + abs(dx) for dy in range(-4, 5) for dx in range(-4, 5) if clear(y + dy, x + dx)] or [9])
             for x in range(W)] for y in range(H)]
    bg = []
    for y in range(H):
        row = []
        for x in range(W):
            d = math.hypot((x - 52) / 64, (y - 32) / 40)
            t = min(1.0, d)
            c = (int(120 * (1 - t) + 26 * t), int(165 * (1 - t) + 50 * t), int(215 * (1 - t) + 98 * t))
            disc = math.hypot(x - 52, (y - 36) * 1.1)  # Mario's white disc behind the emblem
            k = max(0.0, min(1.0, (31 - disc) / 4))
            c = tuple(int(c[j] * (1 - k) + (235, 240, 245)[j] * k) for j in range(3))
            row.append(c + (255,))
        bg.append(row)
    big, w, h = fit(head, 76)
    body = gx.over(bg, big, 52 - w // 2, H - h + 10)
    out = [[(0, 0, 0, 0) if tpl[y][x][3] == 0 else tpl[y][x] if edge[y][x] <= 4 else body[y][x][:3] + (tpl[y][x][3],)
            for x in range(W)] for y in range(H)]
    return {'lowerthird_superteam': gx.encode_cmpr(out, ref)}


if __name__ == '__main__':
    t = build(*sys.argv[1:4])
    os.makedirs('png', exist_ok=True)  # previews, in the working folder
    gx.png('png/superteam_portrait.png', gx.decode(t['captain_superteam_s']))
    gx.png('png/superteam_right.png', gx.resize(gx.decode(t['superteam_right']), 128, 128))
    gx.png('png/superteam_logos.png', gx.decode(t['logos_TEAM_superteam']))
    gx.png('png/superteam_pda.png', gx.decode(t['attributes_superteam']))
