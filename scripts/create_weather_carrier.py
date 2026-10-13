"""Build an editable EDA Pro review project from the exported reference.

Socket geometry and pin order follow the supplied 44-pin development-board photos.
This creates placement and electrical connectivity, not manufacturing approval.
"""
import copy
import csv
import json
from pathlib import Path
import uuid
import zipfile

ROOT = Path(__file__).resolve().parents[1]
OUT = ROOT / "hardware/weather-carrier"
OUT.mkdir(parents=True, exist_ok=True)
source = zipfile.ZipFile(ROOT / "reference-carrier.epro")
project = json.loads(source.read("project.json"))
files = {n: source.read(n) for n in source.namelist() if not n.endswith("/")}


def records(name):
    return [json.loads(x) for x in files[name].decode().splitlines() if x]


def encode(rows):
    return "\n".join(json.dumps(r, ensure_ascii=False, separators=(",", ":"))
                     for r in rows) + "\n"


def uid(name):
    return uuid.uuid5(uuid.NAMESPACE_URL, "weather-carrier/" + name).hex


old_sch = "e9de148a4c3c42f5b615fbf3eef8a029"
old_pcb = "8ae41639eed84e82a59740bce5b0225f"
sref = records(f"SHEET/{old_sch}/1.esch")
bref = records(f"PCB/{old_pcb}.epcb")
sch_id, pcb_id, sheet_id = uid("schematic"), uid("pcb"), uid("sheet")
project["config"]["title"] = "ESP32-S3 AirPlay Weather Carrier R0 REVIEW"
project["boards"] = {"Weather Carrier R0": {"schematic": sch_id, "pcb": pcb_id}}
project["schematics"] = {sch_id: {"name": "Weather Carrier R0", "sheets": [
    {"uuid": sheet_id, "id": 1, "name": "P1", "display_title": "P1"}]}}
project["pcbs"] = {pcb_id: "Weather Carrier R0"}
project["panels"] = {}

# Reuse catalog symbols/footprints, creating honest generic assembly variants
# for passives whose value differs from a catalog item.
def variant(base, name, value):
    dev = copy.deepcopy(project["devices"][base])
    key = uid(name)
    dev.update(uuid=key, title=name, source="", version="1")
    dev["attributes"] = {
        "Symbol": dev["attributes"]["Symbol"],
        "Footprint": dev["attributes"]["Footprint"],
        "Name": name, "Value": value, "Designator": "R?",
        "Add into BOM": "yes", "Convert to PCB": "yes",
    }
    project["devices"][key] = dev
    return key


resistor = "99d06e3bf930462ea763a2c6f8b2c4f6"
r0 = variant(resistor, "0R 0805", "0R")
r47 = variant(resistor, "4.7k 0805 DNP", "4.7k DNP")

# Generic 1x6 cable header, derived from the verified 1x8 library geometry.
base8 = "26a1d14b482e4a05a6245c6639558807"
dev6, sym6, fp6 = uid("header6"), uid("symbol6"), uid("footprint6")
sym8 = project["devices"][base8]["attributes"]["Symbol"]
foot8 = project["devices"][base8]["attributes"]["Footprint"]
sym = records(f"SYMBOL/{sym8}.esym")
pin_numbers = {r[2]: int(r[4]) for r in sym if r[0] == "ATTR" and r[3] == "NUMBER"}
remove = {k for k, v in pin_numbers.items() if v > 6}
new_sym = []
for row in copy.deepcopy(sym):
    if row[0] == "PIN" and row[1] in remove:
        continue
    if row[0] == "ATTR" and row[2] in remove:
        continue
    if row[0] == "PART":
        row[1], row[2] = "HEADER_1X6.1", {"BBOX": [-10, -35, 10, 35]}
    if row[0] == "RECT":
        row[3], row[5] = 35, -35
    if row[0] == "PIN":
        row[5] -= 10
    if row[0] == "ATTR" and row[2] in pin_numbers and row[8] is not None:
        row[8] -= 10
    if row[0] == "CIRCLE":
        row[3] = 30
    new_sym.append(row)
files[f"SYMBOL/{sym6}.esym"] = encode(new_sym).encode()
fp = records(f"FOOTPRINT/{foot8}.efoo")
new_fp = [r for r in copy.deepcopy(fp) if r[0] not in ("PAD", "POLY", "PROP")]
for r in new_fp:
    if r[0] == "HEAD":
        r[1].update(uuid=fp6, title="HDR-TH_1X6_P2.54", source="")
for r in fp:
    if r[0] == "PAD" and int(r[5]) <= 6:
        r = copy.deepcopy(r)
        r[6] += 100
        new_fp.append(r)
new_fp.append(["POLY", "outline6", 0, "", 3, 8,
               ["R", -305, 48, 610, 96, 0, 0], 0])
files[f"FOOTPRINT/{fp6}.efoo"] = encode(new_fp).encode()
for group, key, base, title in [("symbols", sym6, sym8, "HEADER_1X6"),
                                ("footprints", fp6, foot8, "HDR-TH_1X6_P2.54")]:
    project[group][key] = copy.deepcopy(project[group][base])
    project[group][key].update(uuid=key, title=title, display_title=title,
                               source="", version="1")
project["devices"][dev6] = {
    "uuid": dev6, "title": "HEADER_1X6", "description": "Carrier cable header",
    "attributes": {"Symbol": sym6, "Footprint": fp6, "Name": "HEADER_1X6",
                   "Designator": "J?", "Add into BOM": "yes", "Convert to PCB": "yes"},
    "symbol_type": 2, "footprint_type": 4, "source": "", "version": "1",
}

def header(count):
    """Derive numbered single-row connectors from an inspected library pin/pad."""
    device, symbol, footprint = [uid(f"{kind}{count}") for kind in
                                  ("header", "symbol", "footprint")]
    sr = records(f"SYMBOL/{sym8}.esym")
    rows = [copy.deepcopy(r) for r in sr if r[0] in
            ("DOCTYPE", "HEAD", "LINESTYLE", "FONTSTYLE")]
    half = (count + 1) * 5
    rows.extend([["PART", f"HEADER_1X{count}.1", {"BBOX": [-10, -half, 10, half]}],
                 ["RECT", "body", -10, half, 10, -half, 0, 0, 0, "st1", 0]])
    template = next(r for r in sr if r[0] == "PIN")
    attrs = [r for r in sr if r[0] == "ATTR" and r[2] == template[1]]
    for n in range(1, count + 1):
        pin = copy.deepcopy(template)
        pin[1], pin[5] = f"pin{n}", (count - 1) * 5 - (n - 1) * 10
        rows.append(pin)
        for a in copy.deepcopy(attrs):
            a[1], a[2] = f"pin{n}_{a[3]}", pin[1]
            if a[3] in ("NAME", "NUMBER"):
                a[4] = str(n)
            if a[8] is not None:
                a[8] += pin[5] - template[5]
            rows.append(a)
    files[f"SYMBOL/{symbol}.esym"] = encode(rows).encode()
    fr = records(f"FOOTPRINT/{foot8}.efoo")
    rows = [copy.deepcopy(r) for r in fr if r[0] not in ("PAD", "POLY", "PROP")]
    for r in rows:
        if r[0] == "HEAD":
            r[1].update(uuid=footprint, title=f"HDR-TH_1X{count}_P2.54", source="")
    original_pads = [r for r in fr if r[0] == "PAD"]
    for n in range(1, count + 1):
        pad = copy.deepcopy(original_pads[0 if n == 1 else 1])
        pad[1], pad[5], pad[6] = f"pad{n}", str(n), (n-1)*100-(count-1)*50
        rows.append(pad)
    rows.append(["POLY", "body", 0, "", 3, 8,
                 ["R", -count*50-5, 48, count*100+10, 96, 0, 0], 0])
    files[f"FOOTPRINT/{footprint}.efoo"] = encode(rows).encode()
    for group, key, base in [("symbols", symbol, sym8), ("footprints", footprint, foot8)]:
        project[group][key] = copy.deepcopy(project[group][base])
        project[group][key].update(uuid=key, title=f"HEADER_1X{count}_P2.54",
                                   display_title=f"HEADER_1X{count}_P2.54", source="")
    project["devices"][device] = copy.deepcopy(project["devices"][dev6])
    project["devices"][device].update(uuid=device, title=f"HEADER_1X{count}")
    project["devices"][device]["attributes"].update(Symbol=symbol, Footprint=footprint,
                                                   Name=f"HEADER_1X{count}")
    return device


header22, header9 = header(22), header(9)
sch = [copy.deepcopy(r) for r in sref if r[0] in
       ("DOCTYPE", "HEAD", "LINESTYLE", "FONTSTYLE")]
pcb = [copy.deepcopy(r) for r in bref if r[0] in
       ("DOCTYPE", "HEAD", "CANVAS", "LAYER", "LAYER_PHYS", "ACTIVE_LAYER",
        "PREFERENCE", "RULE", "RULE_SELECTOR", "FONT", "PRIMITIVE", "SILK_OPTS")]
pcb.append(["POLY", "board_outline", 0, "", 11, 10,
            ["R", 0, 0, 3150, 3150, 0, 40], 0])
parts, pads = [], []
counter = 30000


def eid():
    global counter
    counter += 1
    return f"e{counter}"


def text(x, y, value, large=False):
    sch.append(["TEXT", eid(), x, y, 0, value, "st6" if large else "st5"])


def label(x, y, net, dx=-60):
    wid = eid()
    sch.append(["WIRE", wid, [[x, y, x + dx, y]], "st7", 0])
    sch.append(["ATTR", eid(), wid, "NET", net, 0, 1,
                x + dx, y, 0, "st8", 0])


def add(ref, device, name, sx, sy, px, py, nets, dnp=False):
    dev = project["devices"][device]
    symid, fpid = dev["attributes"]["Symbol"], dev["attributes"]["Footprint"]
    sr, fr = records(f"SYMBOL/{symid}.esym"), records(f"FOOTPRINT/{fpid}.efoo")
    sid, bid = eid(), eid()
    part_name = next(r[1] for r in sr if r[0] == "PART")
    unique = "WC_" + ref
    sch.append(["COMPONENT", sid, part_name, sx, sy, 0, 0, {}, 0])
    for key, value, visible, x, y in [
            ("Device", device, 0, sx, sy), ("Symbol", symid, 0, None, None),
            ("Designator", ref, 1, sx - 10, sy + 60),
            ("Name", name, 1, sx - 10, sy - (len(nets) * 5 + 22)),
            ("Unique ID", unique, 0, sx, sy)]:
        if ref in ("J1", "J2") and key == "Designator":
            y = sy + 120
        sch.append(["ATTR", eid(), sid, key, value, 0, visible, x, y, 0, "st5", 0])
    numbers = {r[2]: r[4] for r in sr if r[0] == "ATTR" and r[3] == "NUMBER"}
    for pin in sr:
        if pin[0] == "PIN":
            number = numbers[pin[1]]
            if nets.get(number):
                label(sx + pin[4], sy + pin[5], nets[number],
                      -70 if pin[4] < 0 else 70)
    pcb.append(["COMPONENT", bid, 0, 1, px, py, 0,
                {"Unique ID": unique, "Channel ID": "$1" + sid,
                 "CBB Title": "", "Group ID": ""}, 0])
    for key, value, visible in [("Footprint", fpid, 0), ("Device", device, 0),
                                 ("Designator", ref, 1)]:
        pcb.append(["ATTR", eid(), 0, bid, 3, px, py + 80, key, value,
                    0, visible, "default", 40, 6, 0, 0, 3, 0, 0, 0, 0, False])
    for pad in fr:
        if pad[0] == "PAD":
            net = nets.get(pad[5], "")
            pcb.append(["PAD_NET", bid, pad[5], net, pad[1], 0])
            pads.append({"ref": ref, "pin": pad[5], "net": net,
                         "x": px + pad[6], "y": py + pad[7],
                         "layer": pad[4], "shape": pad[10], "hole": pad[9]})
    parts.append({"ref": ref, "name": name, "device": device,
                  "footprint": fpid, "dnp": dnp, "nets": nets})


def pinmap(seq):
    return {str(i + 1): v for i, v in enumerate(seq)}


header20 = "e862e9ce3a5b4150a7020daa0a33030e"
header4 = "1f069cf374544f42831924984f5a0f7e"
# Top/component view: antenna left, USB right. Pin 1 at antenna end.
# J1 is the lower row, J2 the upper row. IO35..37 never leave their socket pads.
left = ['V3.3', 'V3.3', 'ESP_EN', 'IO4', 'IO5', 'IO6', 'IO7', 'IO15',
        'IO16', 'IO17', 'IO18', 'IO8', 'IO3', 'IO46', 'IO9', 'IO10',
        'IO11', 'IO12', 'IO13', 'IO14', 'V5', 'GND']
right = ['GND', 'IO43', 'IO44', 'IO1', 'IO2', 'IO42', 'IO41', 'IO40',
         'IO39', 'IO38', 'RESERVED_IO37', 'RESERVED_IO36', 'RESERVED_IO35',
         'IO0', 'IO45', 'IO48', 'IO47', 'IO21', 'IO20', 'IO19', 'GND', 'GND']
text(100, 1310, "ESP32-S3 AIRPLAY + WEATHER / R0 REVIEW", True)
text(100, 1250, "2x22 SOCKETS / PITCH 2.54mm / ROW CENTERS 25.4mm / PIN1 AT ANTENNA")
text(100, 1225, "Cable headers below use CARRIER pin order. Match signal labels at the module end.")
add('J1', header22, 'ESP LEFT ROW 1x22', 250, 1030, 1125, -1165.1, pinmap(left))
add('J2', header22, 'ESP RIGHT ROW 1x22', 550, 1030, 1125, -165.1, pinmap(right))
text(720, 1180, "PCM5102A / 5V MODULE", True)
add('J3', dev6, 'DAC CABLE 1x6', 1050, 1040, 1500, -1570,
    pinmap(['DAC_SCK', 'DAC_BCK', 'DAC_DIN', 'DAC_LRCK', 'GND', 'V5']))
for i, (net, dest) in enumerate([('IO38', 'DAC_BCK'), ('IO39', 'DAC_LRCK'),
                                ('IO40', 'DAC_DIN'), ('IO41', 'DAC_SCK')]):
    add('R' + str(i + 1), r0, '0R' if i < 3 else '0R DNP / MCLK',
        800, 1080 - i * 75, 1250 + i * 160, -1320,
        {'1': net, '2': dest}, i == 3)
text(710, 755, "R4 DNP by default: check module SCK straps before fitting.")
text(710, 730, "FMT=I2S; XSMT enabled on module. Module VIN=5V confirmed by user.")
text(100, 720, "GDEY075Z08 / DRIVER CABLE", True)
add('J4', header9, 'EPD V1.4 CABLE 1x9 / 3V3', 370, 600, 650, -1910,
    pinmap(['GND', 'V3.3', 'IO12', 'IO11', 'IO16', 'IO17', 'IO10', 'IO7', 'IO4']))
text(100, 495, "Pins 1..9: GND 3V3 SCK SDA RST DC CS1 BUSY CS2(IO4)")
text(100, 470, "MISO unused. GPIO6 is logic enable only; screen supply always on.")
text(650, 650, "BME280 / OPTIONAL", True)
add('J5', header4, 'BME280 / 3V3', 850, 535, 450, -2600,
    pinmap(['V3.3', 'GND', 'IO9', 'IO8']))
add('R5', r47, '4.7k DNP', 1070, 580, 350, -2340, {'1': 'V3.3', '2': 'IO8'}, True)
add('R6', r47, '4.7k DNP', 1070, 480, 550, -2340, {'1': 'V3.3', '2': 'IO9'}, True)
text(650, 425, "CSB=3V3, SDO=GND, address 0x76. Avoid duplicate pull-ups.")
text(100, 385, "PAGE BUTTON / GPIO15", True)
add('SW1', '3e0da1c6de1f4227acfe4bb9989b88ea', 'PAGE', 280, 280,
    2250, -2500, {'1': 'GND', '2': 'IO15'})
add('J6', header4, 'AUX: GND PAGE EN 3V3', 600, 270, 2500, -2080,
    pinmap(['GND', 'IO15', 'IO6', 'V3.3']))
text(100, 170, "J6: external PAGE switch between pins 1/2. GPIO6 pin3 is not a supply.")
text(100, 145, "Native USB GPIO19/20 untouched. No GPIO-powered ground. One power source.")
text(710, 350, "LOCAL SUPPLY BYPASS", True)
for ref, device, name, sx, sy, px, py, rail in [
    ('C1', '27e59dd1acf94f70b80fae573d4fc558', '100nF', 810, 270, 1100, -1750, 'V5'),
    ('C2', '160c15f091d94199a0bf7f2ce0cd2cff', '10uF', 1100, 270, 1350, -1750, 'V5'),
    ('C3', '27e59dd1acf94f70b80fae573d4fc558', '100nF', 810, 150, 350, -2150, 'V3.3'),
    ('C4', '160c15f091d94199a0bf7f2ce0cd2cff', '10uF', 1100, 150, 650, -2150, 'V3.3')]:
    add(ref, device, name, sx, sy, px, py, {'1': rail, '2': 'GND'})

for net in sorted({p['net'] for p in pads if p['net']}):
    pcb.append(['NET', net, None, None, 1, None, 0, None])


def silk(x, y, message, size=45, layer=3):
    pcb.append(['STRING', eid(), 0, layer, x, y, message, 'default', size,
                6, 0, 0, 3, 0, 0, 0, 0, 0])


silk(200, -2870, 'AIRPLAY + WEATHER R0', 70)
silk(200, -2990, 'REVIEW ONLY - VERIFY SOCKET PINOUT', 45)
silk(2400, -370, '2x22\n25.4mm\nANT <\nUSB >', 45)
silk(1120, -1480, 'PCM5102A / 5V', 45)
silk(210, -1810, 'EPD / 3V3', 45)
silk(260, -2750, 'BME280 / 3V3', 40)
silk(2130, -2660, 'PAGE', 45)
silk(2300, -1940, 'AUX', 40)
silk(1870, -1240, 'R4 DNP', 40)
for ref, names in [('J3', ['SCK', 'BCK', 'DIN', 'LCK', 'GND', 'VIN']),
                   ('J4', ['GND', '3V3', 'SCK', 'SDA', 'RST', 'DC', 'CS1', 'BUSY', 'CS2']),
                   ('J5', ['3V3', 'GND', 'SCL', 'SDA']),
                   ('J6', ['GND', 'PAGE', 'EN', '3V3'])]:
    for pad in [p for p in pads if p['ref'] == ref]:
        silk(pad['x'] - 25, pad['y'] - 65, names[int(pad['pin']) - 1], 27)
# The exact antenna overhang cannot be established from memory capacity.
# No copper pours are generated before the mechanical pinout is confirmed.

files = {k: v for k, v in files.items()
         if not k.startswith(('SHEET/', 'PCB/', 'POUR/', 'INSTANCE/'))}
files[f'SHEET/{sch_id}/1.esch'] = encode(sch).encode()
files[f'PCB/{pcb_id}.epcb'] = encode(pcb).encode()
files['project.json'] = json.dumps(project, ensure_ascii=False, indent=2).encode()
with zipfile.ZipFile(OUT / 'weather-carrier-r0.epro', 'w', zipfile.ZIP_DEFLATED) as dest:
    for name, content in files.items():
        dest.writestr(name, content)
(OUT / 'connectivity.json').write_text(json.dumps({'parts': parts, 'pads': pads},
                                                indent=2), encoding='utf-8')
with (OUT / 'connector-pinout.csv').open('w', newline='', encoding='utf-8-sig') as f:
    writer = csv.writer(f)
    writer.writerow(['Connector', 'Pin', 'Net', 'Status'])
    for part in parts:
        if part['ref'].startswith('J'):
            for pin, net in part['nets'].items():
                writer.writerow([part['ref'], pin, net,
                    'Photo verified; pin1 antenna end' if part['ref'] in ['J1', 'J2']
                    else 'Carrier cable pin order; match by signal'])
with (OUT / 'bom.csv').open('w', newline='', encoding='utf-8-sig') as f:
    writer = csv.writer(f)
    writer.writerow(['Reference', 'Value', 'Populate', 'Footprint'])
    for part in parts:
        writer.writerow([part['ref'], part['name'], 'DNP' if part['dnp'] else 'Yes',
                         project['footprints'][part['footprint']]['title']])
assert len({p['ref'] for p in parts}) == len(parts)
assert all(p['net'] for p in pads)
for ref in ('J1', 'J2'):
    row = [p for p in pads if p['ref'] == ref]
    assert len(row) == 22 and row[-1]['x'] - row[0]['x'] == 2100
assert abs(abs(next(p['y'] for p in pads if p['ref'] == 'J1') -
               next(p['y'] for p in pads if p['ref'] == 'J2')) - 1000) < 1e-6
print(f'Created {len(parts)} components, {len(pads)} pads: {OUT / "weather-carrier-r0.epro"}')
