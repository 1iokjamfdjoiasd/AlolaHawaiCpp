# Gera os ícones de botão do controle (Xbox / PlayStation) que substituem os ícones de botão do jogo (texturas do 3DS).
# Saída: bin/icones_controle/<xbox|ps>/<HASH>_<codigo>.png, um por ícone do jogo x botão do controle (o porte escolhe o
# arquivo do botão em que o usuário mapeou aquele botão do 3DS). Códigos = SDL_GameControllerButton; 64 = LT, 65 = RT.
# Uso: python gerar_icones_controle.py [pasta_saida]
import os, sys, math
from PIL import Image, ImageDraw, ImageFont

OUT = sys.argv[1] if len(sys.argv) > 1 else os.path.join(os.path.dirname(__file__), '..', 'bin', 'icones_controle')
S = 4  # escala (16x16 do jogo -> 64x64)

# ícones do jogo: hash (novo, como o Azahar grava) -> (largura, altura, tipo, girado)
GAME = {
    '0E8403331BFE0087': (16, 16, 'circulo', 0),    # A
    '65C87B93AF6B164B': (16, 16, 'circulo', 0),    # B
    'B3F60FFA9BF73C9C': (16, 16, 'circulo', 0),    # X
    '89AE7C33AE138E83': (16, 16, 'circulo', 0),    # Y (reto como o X; conferido no jogo 23/11: com -90 saía deitado, com 180 de cabeça para baixo)
    'D0917714C6CBEFED': (32, 32, 'etiqueta', 0),   # R (etiqueta laranja)
    'A0379692CF9CF7DD': (64, 64, 'balao', 0),      # A no balão sobre portas/pessoas (só um canto da textura, deitado)
}
# SDL: 0 A(baixo) 1 B(direita) 2 X(esquerda) 3 Y(cima) 4 Back 6 Start 9 LB 10 RB 11-14 direcional; 64 LT 65 RT
CODES = [0, 1, 2, 3, 4, 6, 9, 10, 11, 12, 13, 14, 64, 65]

XBOX = {0: ('A', (16, 185, 50)), 1: ('B', (230, 40, 40)), 2: ('X', (30, 110, 235)), 3: ('Y', (250, 200, 0)),
        4: ('view', None), 6: ('menu', None), 9: ('LB', None), 10: ('RB', None), 64: ('LT', None), 65: ('RT', None)}
PS = {0: ('cruz', (90, 150, 255)), 1: ('bola', (255, 70, 80)), 2: ('quadrado', (240, 120, 210)), 3: ('triangulo', (40, 210, 150)),
      4: ('create', None), 6: ('options', None), 9: ('L1', None), 10: ('R1', None), 64: ('L2', None), 65: ('R2', None)}

def font(px):
    for f in ('arialbd.ttf', 'seguisb.ttf', 'arial.ttf'):
        try: return ImageFont.truetype(f, px)
        except OSError: pass
    return ImageFont.load_default()

def centered_text(d, box, txt, px, fill):
    f = font(px)
    l, t, r, b = d.textbbox((0, 0), txt, font=f)
    x = (box[0] + box[2] - (r - l)) / 2 - l
    y = (box[1] + box[3] - (b - t)) / 2 - t
    d.text((x, y), txt, font=f, fill=fill)

def symbol(d, kind, box, col, w):
    x0, y0, x1, y1 = box
    cx, cy = (x0 + x1) / 2, (y0 + y1) / 2
    r = (x1 - x0) / 2
    if kind == 'cruz':
        k = r * 0.55
        d.line((cx - k, cy - k, cx + k, cy + k), fill=col, width=w); d.line((cx - k, cy + k, cx + k, cy - k), fill=col, width=w)
    elif kind == 'bola':
        k = r * 0.55; d.ellipse((cx - k, cy - k, cx + k, cy + k), outline=col, width=w)
    elif kind == 'quadrado':
        k = r * 0.5; d.rectangle((cx - k, cy - k, cx + k, cy + k), outline=col, width=w)
    elif kind == 'triangulo':
        k = r * 0.6
        pts = [(cx, cy - k), (cx + k * 0.95, cy + k * 0.6), (cx - k * 0.95, cy + k * 0.6)]
        d.line(pts + [pts[0]], fill=col, width=w, joint='curve')

def dpad(d, box, which):
    x0, y0, x1, y1 = box
    cx, cy = (x0 + x1) / 2, (y0 + y1) / 2
    a = (x1 - x0) * 0.16
    L = (x1 - x0) * 0.42
    d.rectangle((cx - a, cy - L, cx + a, cy + L), fill=(235, 235, 235)); d.rectangle((cx - L, cy - a, cx + L, cy + a), fill=(235, 235, 235))
    hi = (255, 255, 255)
    off = {11: (0, -1), 12: (0, 1), 13: (-1, 0), 14: (1, 0)}[which]
    hx, hy = cx + off[0] * L * 0.62, cy + off[1] * L * 0.62
    d.rectangle((hx - a * 0.9, hy - a * 0.9, hx + a * 0.9, hy + a * 0.9), fill=(40, 40, 40))

def draw_mono(style, code, size):
    """game-style icon: a black disc and the symbol in white (the game's menus colour it, like the 3DS icon)"""
    W = size
    im = Image.new('RGBA', (W, W), (0, 0, 0, 0))
    d = ImageDraw.Draw(im)
    blk, wht = (0, 0, 0, 255), (255, 255, 255, 255)
    d.ellipse((0, 0, W - 1, W - 1), fill=blk)
    face = XBOX if style == 'xbox' else PS
    if code in (0, 1, 2, 3):
        lab = face[code][0]
        if style == 'xbox': centered_text(d, (0, 0, W, W), lab, int(W * 0.72), wht)
        else: symbol(d, lab, (W * 0.1, W * 0.1, W * 0.9, W * 0.9), wht, max(2, int(W * 0.13)))
    elif code in (11, 12, 13, 14):
        x0, y0, x1, y1 = W * 0.14, W * 0.14, W * 0.86, W * 0.86
        cx, cy = W / 2, W / 2; a = W * 0.12; L = W * 0.36
        d.rectangle((cx - a, cy - L, cx + a, cy + L), fill=wht); d.rectangle((cx - L, cy - a, cx + L, cy + a), fill=wht)
        off = {11: (0, -1), 12: (0, 1), 13: (-1, 0), 14: (1, 0)}[code]
        hx, hy = cx + off[0] * L * 0.62, cy + off[1] * L * 0.62
        d.rectangle((hx - a * 0.8, hy - a * 0.8, hx + a * 0.8, hy + a * 0.8), fill=blk)
    else:
        lab = face[code][0]
        txt = {'view': 'V', 'menu': 'M', 'create': 'C', 'options': 'O'}.get(lab, lab)
        if lab == 'view':
            w = max(1, int(W * 0.08))
            d.rectangle((W * 0.24, W * 0.3, W * 0.56, W * 0.6), outline=wht, width=w); d.rectangle((W * 0.42, W * 0.42, W * 0.74, W * 0.72), outline=wht, width=w)
        elif lab == 'menu':
            for k in range(3): d.line((W * 0.26, W * (0.34 + k * 0.16), W * 0.74, W * (0.34 + k * 0.16)), fill=wht, width=max(1, int(W * 0.09)))
        else:
            centered_text(d, (0, 0, W, W), txt, int(W * (0.62 if len(txt) == 1 else 0.44)), wht)
    return im

def draw_button(style, code, size):
    """um ícone redondo/arredondado do botão `code` no estilo, ocupando a imagem (tamanho `size`)"""
    W = size
    im = Image.new('RGBA', (W, W), (0, 0, 0, 0))
    d = ImageDraw.Draw(im)
    m = W * 0.04
    face = XBOX if style == 'xbox' else PS
    dark, rim = (28, 28, 30, 255), (255, 255, 255, 255)
    if code in (0, 1, 2, 3):
        d.ellipse((m, m, W - m, W - m), fill=dark, outline=rim, width=max(1, int(W * 0.05)))
        lab, col = face[code]
        if style == 'xbox': centered_text(d, (0, 0, W, W), lab, int(W * 0.62), col + (255,))
        else: symbol(d, lab, (W * 0.18, W * 0.18, W * 0.82, W * 0.82), col + (255,), max(2, int(W * 0.11)))
    elif code in (11, 12, 13, 14):
        d.ellipse((m, m, W - m, W - m), fill=dark, outline=rim, width=max(1, int(W * 0.05)))
        dpad(d, (W * 0.12, W * 0.12, W * 0.88, W * 0.88), code)
    elif code in (4, 6):
        d.rounded_rectangle((m, W * 0.2, W - m, W * 0.8), radius=W * 0.3, fill=dark, outline=rim, width=max(1, int(W * 0.05)))
        lab = face[code][0]
        if lab == 'view':
            d.rectangle((W * 0.3, W * 0.35, W * 0.55, W * 0.58), outline=rim, width=max(1, int(W * 0.05)))
            d.rectangle((W * 0.42, W * 0.43, W * 0.68, W * 0.66), outline=rim, width=max(1, int(W * 0.05)))
        elif lab == 'menu':
            for k in range(3): d.line((W * 0.3, W * (0.38 + k * 0.12), W * 0.7, W * (0.38 + k * 0.12)), fill=rim, width=max(1, int(W * 0.06)))
        else:
            centered_text(d, (0, W * 0.2, W, W * 0.8), 'OPT' if lab == 'options' else 'CRE', int(W * 0.3), rim)
    else:   # ombros / gatilhos
        d.rounded_rectangle((m, W * 0.18, W - m, W * 0.82), radius=W * 0.18, fill=dark, outline=rim, width=max(1, int(W * 0.05)))
        centered_text(d, (0, W * 0.18, W, W * 0.82), face[code][0], int(W * 0.42), rim)
    return im

def draw_tag(style, code, w, h):
    """a etiqueta laranja (ombro R do jogo) com o nome do botão"""
    im = Image.new('RGBA', (w, h), (0, 0, 0, 0))
    d = ImageDraw.Draw(im)
    d.rounded_rectangle((w * 0.1, h * 0.12, w * 0.9, h * 0.88), radius=w * 0.12, fill=(250, 150, 30, 255))
    face = XBOX if style == 'xbox' else PS
    lab = face.get(code, (None,))[0]
    if code in (0, 1, 2, 3):
        b = draw_button(style, code, int(h * 0.7))
        im.alpha_composite(b, (int((w - b.width) / 2), int((h - b.height) / 2)))
    elif code in (11, 12, 13, 14):
        b = draw_button(style, code, int(h * 0.7))
        im.alpha_composite(b, (int((w - b.width) / 2), int((h - b.height) / 2)))
    else:
        txt = {'view': 'VIEW', 'menu': 'MENU', 'create': 'CRE', 'options': 'OPT'}.get(lab, lab)
        centered_text(d, (0, 0, w, h), txt, int(h * (0.5 if len(txt) <= 2 else 0.24)), (255, 255, 255, 255))
    return im

def draw_balloon(style, code, w, h):
    """the 'press A' balloon over doors / people: drawn in the texture's own corner and lying down (tail to the left,
    symbol turned), like the game's texture (x 0-28, y 21-43 of 64x64); inside, the controller's button"""
    k = w / 64.0
    im = Image.new('RGBA', (w, h), (0, 0, 0, 0))
    d = ImageDraw.Draw(im)
    cx, cy, r = 15 * k, 32 * k, 10.5 * k
    d.polygon([(0.5 * k, cy), (cx - r * 0.55, cy - r * 0.62), (cx - r * 0.55, cy + r * 0.62)], fill=(0, 0, 0, 255))
    d.ellipse((cx - r, cy - r, cx + r, cy + r), fill=(0, 0, 0, 255))
    r2 = r - 1.4 * k
    d.polygon([(2.6 * k, cy), (cx - r2 * 0.5, cy - r2 * 0.5), (cx - r2 * 0.5, cy + r2 * 0.5)], fill=(255, 255, 255, 255))
    d.ellipse((cx - r2, cy - r2, cx + r2, cy + r2), fill=(255, 255, 255, 255))
    rb = int(r2 * 1.55)
    b = draw_button(style, code, rb * 2 // 1 if rb > 0 else 8)
    b = b.resize((int(r2 * 1.7), int(r2 * 1.7)), Image.LANCZOS).rotate(-90, expand=False)   # (lying like the balloon)
    im.alpha_composite(b, (int(cx - b.width / 2), int(cy - b.height / 2)))
    return im

def main():
    n = 0
    for style in ('xbox', 'ps'):
        os.makedirs(os.path.join(OUT, style), exist_ok=True)
        for hsh, (w, h, kind, rot) in GAME.items():
            for code in CODES:
                if kind == 'circulo':
                    im = draw_mono(style, code, w * S)
                elif kind == 'balao':
                    im = draw_balloon(style, code, w * S, h * S)
                else:
                    im = draw_tag(style, code, w * S, h * S)
                if rot: im = im.rotate(rot, expand=False)
                # the Azahar's custom-texture path shows the PNG turned 90 degrees anticlockwise (measured with an arrow on
                # the ride menu's B icon): turned clockwise here so it ends up as the game draws its own
                if kind == 'circulo':   # (only these 16x16 IA4 icons: the balloon and the R tag, RGBA4, are not turned)
                    im = im.rotate(-90, expand=False)
                # (upright, like the textures the Azahar dumps: it flips them back itself when loading)
                im.save(os.path.join(OUT, style, f'{hsh}_{code}.png'))
                n += 1
    print(n, 'ícones em', os.path.abspath(OUT))

if __name__ == '__main__':
    main()
