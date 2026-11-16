#!/usr/bin/env python3
"""Gera pistar/countries.json e as bandeiras pistar/flags/<cc>.565 (RGB565 60x40, little-endian).

Uso (na máquina de quem mantém o projeto, NÃO no Pi-Star):
    pip install pillow
    python3 tools/make_flags.py

countries.json:
  name2cc : nome do país como vem no RadioID (normalizado) -> ISO 3166-1 alfa-2
  mcc     : 3 primeiros dígitos do DMR ID (MCC) -> ISO2   (reserva, se o país não vier do RadioID)
  names   : ISO2 -> nome em inglês

Fontes:
- Bandeiras e nomes: https://flagcdn.com (imagens de domínio público, Wikimedia Commons)
- MCC -> país: https://github.com/pbakondy/mcc-mnc-list (MIT)
"""
import collections, io, json, os, sys, unicodedata, urllib.request

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
OUT = os.path.join(ROOT, "pistar")
W, H = 60, 40
MCC_URL = "https://raw.githubusercontent.com/pbakondy/mcc-mnc-list/master/mcc-mnc-list.json"
CODES_URL = "https://flagcdn.com/en/codes.json"

# grafias do RadioID que não batem com os nomes do flagcdn
OVERRIDES = {
    "turkiye": "tr", "korea republic of": "kr", "argentina republic": "ar", "czech republic": "cz",
    "bosnia and hercegovina": "ba", "luxemburg": "lu", "macedonia": "mk", "us virgin islands": "vi",
    "corsica": "fr", "bonaire": "bq", "ivory coast": "ci", "brunei darussalam": "bn", "macao": "mo",
    "aaland islands": "ax", "swasiland": "sz", "mauretania": "mr", "ascension island": "sh",
    "united states": "us", "united kingdom": "gb", "russia": "ru", "south korea": "kr",
}


def norm(s):
    s = unicodedata.normalize("NFKD", s).encode("ascii", "ignore").decode().lower()
    return s.replace("&", "and").replace(".", "").strip()


def get(url):
    req = urllib.request.Request(url, headers={"User-Agent": "pistar-cyd-panel/1.0"})
    with urllib.request.urlopen(req, timeout=20) as r:
        return r.read()


def main():
    from PIL import Image
    codes = {k: v for k, v in json.loads(get(CODES_URL)).items() if len(k) == 2}
    name2cc = {norm(v): k for k, v in codes.items()}
    name2cc.update(OVERRIDES)

    votes = collections.defaultdict(collections.Counter)
    for r in json.loads(get(MCC_URL)):
        mcc, cc = str(r.get("mcc") or ""), (r.get("countryCode") or "").lower()
        if len(mcc) == 3 and mcc.isdigit() and cc in codes:
            votes[mcc][cc] += 1
    mcc = {k: v.most_common(1)[0][0] for k, v in sorted(votes.items())}

    with open(os.path.join(OUT, "countries.json"), "w") as f:
        json.dump({"name2cc": name2cc, "mcc": mcc, "names": codes}, f, indent=0, sort_keys=True)

    fdir = os.path.join(OUT, "flags")
    os.makedirs(fdir, exist_ok=True)
    ok = 0
    for cc in sorted(codes):
        try:
            img = Image.open(io.BytesIO(get(f"https://flagcdn.com/w160/{cc}.png"))).convert("RGBA")
            bg = Image.new("RGBA", img.size, (255, 255, 255, 255))
            img = Image.alpha_composite(bg, img).convert("RGB").resize((W, H), Image.LANCZOS)
            buf = bytearray()
            for r, g, b in img.getdata():
                v = ((r & 0xF8) << 8) | ((g & 0xFC) << 3) | (b >> 3)
                buf += bytes((v & 0xFF, v >> 8))
            with open(os.path.join(fdir, cc + ".565"), "wb") as f:
                f.write(buf)
            ok += 1
        except Exception as e:
            print("sem bandeira:", cc, e, file=sys.stderr)
    print(f"{len(name2cc)} nomes, {len(mcc)} MCCs, {ok} bandeiras")


if __name__ == "__main__":
    main()
