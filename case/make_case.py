"""
Token Buddy - case a robottino pixel, script per Autodesk Fusion.

Disegna il frontale (con tasca, ganci e piedini inclinati di 15 gradi) e il
coperchio a griglia che si incastra a scatto, poi esporta gli STL in questa
cartella. Si lancia da Fusion: Utilita' > Script e componenti aggiuntivi.
Cancella la timeline del documento attivo: usalo in un documento vuoto.

Scegli la scheda cambiando BOARD. Misure in cm (0.01 cm = 0.1 mm).
"""
import adsk.core, adsk.fusion, math, os

BOARD = 'cyd28'   # 'cyd28' oppure 'cyd24'

# guardando lo schermo con la presa USB-C a destra
BOARDS = {
    'cyd28': dict(pcb=(8.60, 5.00), glass_to_pcb_back=0.96, total=1.147,
                  win=(5.84, 4.40), win_off=(-0.178, 0.0), usb_from_bottom=2.33,
                  hooks_x=2.5, legs=(1.6, 2.8, 3.4), grid=(9, 5)),
    'cyd24': dict(pcb=(7.423, 4.322), glass_to_pcb_back=0.581, total=1.14,
                  win=(5.04, 3.80), win_off=(-0.25, -0.03), usb_from_bottom=1.713,
                  hooks_x=2.0, legs=(1.4, 2.5, 3.0), grid=(8, 5)),
}
B = BOARDS[BOARD]
SUFFIX = '_' + BOARD

VI = adsk.core.ValueInput.createByReal
P = adsk.core.Point3D.create
V = adsk.core.Vector3D.create
NEW = adsk.fusion.FeatureOperations.NewBodyFeatureOperation
JOIN = adsk.fusion.FeatureOperations.JoinFeatureOperation
CUT = adsk.fusion.FeatureOperations.CutFeatureOperation

PLATE = 0.3
CLR = 0.03                                  # gioco tasca per lato (0.3 mm)
PW, PH = B['pcb'][0] + 2*CLR, B['pcb'][1] + 2*CLR
WALL = 0.16
OW, OH = PW + 2*WALL, PH + 2*WALL
PCB_BACK = -(PLATE + B['glass_to_pcb_back'])
RAMP_END = PCB_BACK - 0.14                  # fine rampa dei dentini
RIM_Z = min(PCB_BACK - 0.24, -(PLATE + B['total'] + 0.05))   # bordo fin dietro i componenti
WIN_W, WIN_H = B['win']
WIN_X, WIN_Y = B['win_off']
USB_Y, USB_H = -B['pcb'][1]/2 + B['usb_from_bottom'], 1.4
HOOK_XS, HOOK_W, SLIT, LIP = (-B['hooks_x'], B['hooks_x']), 0.9, 0.1, 0.05
GROOVE_Z, GROOVE_D, GROOVE_H = RIM_Z + 0.18, 0.03, 0.06   # scanalatura per lo scatto del coperchio

# sagoma: testa attorno al bordo, orecchie, antenna, gambe e piedi
HW, HH = OW/2 + 0.51, OH/2 + 0.61
LEG_IN, LEG_OUT, FOOT_OUT = B['legs']
FOOT_Y = -HH - 1.4
SHAPES = [(-HW, -HH, HW, HH),
          (-HW - 0.6, -0.6, -HW, 0.6), (HW, -0.6, HW + 0.6, 0.6),
          (-0.2, HH, 0.2, HH + 0.8), (-0.5, HH + 0.8, 0.5, HH + 1.6),
          (-LEG_OUT, -HH - 0.8, -LEG_IN, -HH), (LEG_IN, -HH - 0.8, LEG_OUT, -HH),
          (-FOOT_OUT, FOOT_Y, -LEG_IN, -HH - 0.8), (LEG_IN, FOOT_Y, FOOT_OUT, -HH - 0.8)]

TILT = math.radians(15)
RIB_W, RIB_BACK = 0.4, -4.0
COVER_T, SKIRT_T, SKIRT_H, FIT = 0.12, 0.14, 0.3, 0.012   # 0.12 mm di gioco: incastro a pressione
BEAD, BEAD_LEN = 0.04, 3.0
PIN_D, PIN_PRELOAD = 0.5, 0.02
PIN_X, PIN_Y = B['pcb'][0]/2 - 0.35, B['pcb'][1]/2 - 0.35


def plane_at(c, base, coord, name):
    n = base.geometry.normal
    inp = c.constructionPlanes.createInput()
    inp.setByOffset(base, VI(coord / (n.x + n.y + n.z)))
    pl = c.constructionPlanes.add(inp)
    pl.name = name
    pl.isLightBulbOn = False
    return pl

def rect(sk, x0, y0, x1, y1, z):
    sk.sketchCurves.sketchLines.addTwoPointRectangle(sk.modelToSketchSpace(P(x0, y0, z)), sk.modelToSketchSpace(P(x1, y1, z)))

def poly(sk, pts):
    L = sk.sketchCurves.sketchLines
    sp = [sk.modelToSketchSpace(p) for p in pts]
    lines = []
    for i in range(len(sp)):
        a = lines[-1].endSketchPoint if lines else sp[0]
        b = lines[0].startSketchPoint if i == len(sp) - 1 else sp[i + 1]
        lines.append(L.addByTwoPoints(a, b))

def profiles(sk, keep=None):
    oc = adsk.core.ObjectCollection.create()
    for p in sk.profiles:
        if keep is None or keep(p):
            oc.add(p)
    return oc

def extrude(c, sk, prof, direction, dist, op, body=None):
    n = sk.referencePlane.geometry.normal
    d = adsk.fusion.ExtentDirections.PositiveExtentDirection if n.dotProduct(direction) > 0 else adsk.fusion.ExtentDirections.NegativeExtentDirection
    inp = c.features.extrudeFeatures.createInput(prof, op)
    inp.setOneSideExtent(adsk.fusion.DistanceExtentDefinition.create(VI(dist)), d)
    if body is not None:
        inp.participantBodies = [body]
    return c.features.extrudeFeatures.add(inp)

def symmetric(c, prof, width, op, body):
    inp = c.features.extrudeFeatures.createInput(prof, op)
    inp.setSymmetricExtent(VI(width), True)
    inp.participantBodies = [body]
    return c.features.extrudeFeatures.add(inp)


def build_front(c):
    XY, YZ = c.xYConstructionPlane, c.yZConstructionPlane
    inner = plane_at(c, XY, -PLATE, 'Interno frontale')
    back = plane_at(c, XY, RIM_Z, 'Retro bordo')

    sk = c.sketches.add(XY); sk.name = 'Sagoma robottino'
    for s in SHAPES:
        rect(sk, *s, 0)
    body = extrude(c, sk, profiles(sk), V(0,0,-1), PLATE, NEW).bodies.item(0)
    body.name = 'Robottino CYD'

    sk = c.sketches.add(XY); sk.name = 'Finestra display'
    rect(sk, WIN_X - WIN_W/2, WIN_Y - WIN_H/2, WIN_X + WIN_W/2, WIN_Y + WIN_H/2, 0)
    extrude(c, sk, sk.profiles.item(0), V(0,0,-1), PLATE + 0.01, CUT, body)

    sk = c.sketches.add(inner); sk.name = 'Bordo scheda'
    rect(sk, -OW/2, -OH/2, OW/2, OH/2, -PLATE)
    rect(sk, -PW/2, -PH/2, PW/2, PH/2, -PLATE)
    extrude(c, sk, profiles(sk, lambda p: p.profileLoops.count == 2), V(0,0,-1), -RIM_Z - PLATE, JOIN, body)

    sk = c.sketches.add(back); sk.name = 'Feritoia USB-C'
    rect(sk, PW/2 - 0.05, USB_Y - USB_H/2, OW/2 + 0.05, USB_Y + USB_H/2, RIM_Z)
    extrude(c, sk, sk.profiles.item(0), V(0,0,1), -RIM_Z - PLATE, CUT, body)

    sk = c.sketches.add(back); sk.name = 'Fessure ganci'
    for sy in (-1, 1):
        y0, y1 = sy*(PH/2 - 0.02), sy*(OH/2 + 0.05)
        for hx in HOOK_XS:
            for ex in (hx - HOOK_W/2 - SLIT, hx + HOOK_W/2):
                rect(sk, ex, min(y0, y1), ex + SLIT, max(y0, y1), RIM_Z)
    extrude(c, sk, profiles(sk), V(0,0,1), -RIM_Z - PLATE - 0.15, CUT, body)

    for hx in HOOK_XS:
        pl = plane_at(c, YZ, hx, 'Gancio x=%.1f' % hx)
        sk = c.sketches.add(pl); sk.name = 'Dentini x=%.1f' % hx
        L = sk.sketchCurves.sketchLines
        for sy in (-1, 1):
            wy = sy*PH/2
            a = sk.modelToSketchSpace(P(hx, wy, PCB_BACK - 0.01))
            b = sk.modelToSketchSpace(P(hx, wy - sy*LIP, PCB_BACK - 0.01))
            d = sk.modelToSketchSpace(P(hx, wy, RAMP_END))
            l1 = L.addByTwoPoints(a, b)
            l2 = L.addByTwoPoints(l1.endSketchPoint, d)
            L.addByTwoPoints(l2.endSketchPoint, l1.startSketchPoint)
        symmetric(c, profiles(sk), HOOK_W - 0.04, JOIN, body)

    # scanalatura sui lati lunghi esterni del bordo: ci entra la nervatura del coperchio
    sk = c.sketches.add(YZ); sk.name = 'Scanalatura scatto'
    for sy in (-1, 1):
        y0, y1 = sy*(OH/2 - GROOVE_D), sy*(OH/2 + 0.02)
        sk.sketchCurves.sketchLines.addTwoPointRectangle(sk.modelToSketchSpace(P(0, min(y0, y1), GROOVE_Z - GROOVE_H/2)),
                                                         sk.modelToSketchSpace(P(0, max(y0, y1), GROOVE_Z + GROOVE_H/2)))
    symmetric(c, profiles(sk), OW + 0.2, CUT, body)

    # nervature a cuneo dietro le gambe: con i piedini fanno un appoggio inclinato di 15 gradi
    rise = lambda z: FOOT_Y + (-z - PLATE) * math.tan(TILT)
    rib_top = -(OH/2 + FIT + SKIRT_T + 0.2)
    for hx in HOOK_XS:
        pl = c.constructionPlanes.itemByName('Gancio x=%.1f' % hx)
        sk = c.sketches.add(pl); sk.name = 'Nervatura x=%.1f' % hx
        poly(sk, [P(hx, rise(-PLATE), -PLATE), P(hx, rise(RIB_BACK), RIB_BACK), P(hx, rib_top, RIB_BACK),
                  P(hx, rib_top, RIM_Z + SKIRT_H + 0.1), P(hx, -OH/2, RIM_Z + SKIRT_H + 0.1), P(hx, -OH/2, -PLATE)])
        symmetric(c, sk.profiles.item(0), RIB_W, JOIN, body)
    return body, back


def build_cover(c, back):
    YZ = c.yZConstructionPlane
    cw, ch = OW + 2*(FIT + SKIRT_T), OH + 2*(FIT + SKIRT_T)
    sk = c.sketches.add(back); sk.name = 'Coperchio'
    rect(sk, -cw/2, -ch/2, cw/2, ch/2, RIM_Z)
    cover = extrude(c, sk, sk.profiles.item(0), V(0,0,-1), COVER_T, NEW).bodies.item(0)
    cover.name = 'Coperchio griglia'

    sk = c.sketches.add(back); sk.name = 'Gonna coperchio'
    rect(sk, -cw/2, -ch/2, cw/2, ch/2, RIM_Z)
    rect(sk, -(OW/2 + FIT), -(OH/2 + FIT), OW/2 + FIT, OH/2 + FIT, RIM_Z)
    extrude(c, sk, profiles(sk, lambda p: p.profileLoops.count == 2), V(0,0,1), SKIRT_H, JOIN, cover)

    # nervature a scatto dentro la gonna, sui lati lunghi: entrano nella scanalatura del bordo
    sk = c.sketches.add(YZ); sk.name = 'Nervature scatto'
    for sy in (-1, 1):
        ys = sy*(OH/2 + FIT)
        poly(sk, [P(0, ys, GROOVE_Z - 0.03), P(0, ys - sy*BEAD, GROOVE_Z), P(0, ys, GROOVE_Z + 0.03)])
    symmetric(c, profiles(sk), BEAD_LEN, JOIN, cover)

    # perni che spingono la scheda contro il frontale
    sk = c.sketches.add(back); sk.name = 'Perni coperchio'
    for sx in (-1, 1):
        for sy in (-1, 1):
            sk.sketchCurves.sketchCircles.addByCenterRadius(sk.modelToSketchSpace(P(sx*PIN_X, sy*PIN_Y, RIM_Z)), PIN_D/2)
    extrude(c, sk, profiles(sk), V(0,0,1), PCB_BACK + PIN_PRELOAD - RIM_Z, JOIN, cover)

    sk = c.sketches.add(back); sk.name = 'Griglia pixel'
    nx, ny = B['grid']
    for k in range(nx):
        for j in range(ny):
            if k in (0, nx - 1) and j in (0, ny - 1):
                continue
            x, y = 0.9*(k - (nx - 1)/2), 0.9*(j - (ny - 1)/2)
            rect(sk, x - 0.3, y - 0.3, x + 0.3, y + 0.3, RIM_Z)
    extrude(c, sk, profiles(sk), V(0,0,-1), COVER_T + 0.01, CUT, cover)

    sk = c.sketches.add(back); sk.name = 'Passaggio USB-C coperchio'
    rect(sk, OW/2 - 0.1, USB_Y - USB_H/2, cw/2 + 0.1, USB_Y + USB_H/2, RIM_Z)
    inp = c.features.extrudeFeatures.createInput(sk.profiles.item(0), CUT)
    inp.setSymmetricExtent(VI(0.4), False)
    inp.participantBodies = [cover]
    c.features.extrudeFeatures.add(inp)
    return cover


def run(_context: str):
    app = adsk.core.Application.get()
    design = adsk.fusion.Design.cast(app.activeProduct)
    design.designType = adsk.fusion.DesignTypes.ParametricDesignType
    tl = design.timeline
    tl.moveToBeginning()
    tl.deleteAllAfterMarker()
    c = design.rootComponent

    body, back = build_front(c)
    cover = build_cover(c, back)

    oc = adsk.core.ObjectCollection.create(); oc.add(body); oc.add(cover)
    res = design.analyzeInterference(design.createInterferenceInput(oc))
    print(BOARD, 'interferenze:', res.count)

    # in piedi sui piedini, inclinato all'indietro di 15 gradi
    m = adsk.core.Matrix3D.create()
    m.setToRotation(math.radians(90) - TILT, V(1, 0, 0), P(0, 0, 0))
    mv = c.features.moveFeatures.createInput2(oc)
    mv.defineAsFreeMove(m)
    c.features.moveFeatures.add(mv).name = 'Inclinato 15 gradi'
    zmin = min(b.boundingBox.minPoint.z for b in (body, cover))
    t = adsk.core.Matrix3D.create(); t.translation = V(0, 0, -zmin)
    mv = c.features.moveFeatures.createInput2(oc)
    mv.defineAsFreeMove(t)
    c.features.moveFeatures.add(mv).name = 'Sul piano'

    ap = design.appearances.itemByName('Arancione robottino')
    folder = os.path.dirname(os.path.abspath(__file__)) if '__file__' in globals() else os.path.expanduser('~')
    names = {'Robottino CYD': 'token_buddy_front', 'Coperchio griglia': 'token_buddy_back_cover'}
    for b in (body, cover):
        if ap:
            b.appearance = ap
        path = os.path.join(folder, names[b.name] + SUFFIX + '.stl')
        o = design.exportManager.createSTLExportOptions(b, path)
        o.meshRefinement = adsk.fusion.MeshRefinementSettings.MeshRefinementHigh
        design.exportManager.execute(o)
        print(b.name, 'vol %.1f cm3' % b.volume, '->', path)
    for i in range(tl.count):
        it = tl.item(i)
        if it.healthState != adsk.fusion.FeatureHealthStates.HealthyFeatureHealthState:
            print('PROBLEMA', it.name, it.errorOrWarningMessage)
