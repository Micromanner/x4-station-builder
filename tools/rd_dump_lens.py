# qrenderdoc --python: the 4 frame-start additive quad draws (pipe 29376) in full —
# vertex/instance buffers, all cbuffers, textures — plus raw 3D tonemap LUTs.
import os, json, struct, traceback
import renderdoc as rd
CAPTURE = r'C:\Users\01tee\Downloads\sun.rdc'
OUTDIR = r'C:\Users\01tee\Documents\Projects\x4-station-builder\tools\rd_frame'
EIDS = [1572, 1577, 1582, 1587, 1592, 1597, 1602, 1607, 1612, 1617, 1622, 1627, 1632, 1637, 1642, 1647, 1652, 1657, 1662, 1667, 1672, 1677, 1682, 1687, 1692, 1697, 1702, 1707, 1712, 1717, 1722, 1727, 1732, 1737, 1742, 1747, 1752, 1757, 1762, 1767, 1772, 1777, 1782, 1787, 1792, 1797, 1802, 1807, 1812, 1817, 1822, 1827, 1832, 1837, 1842, 1847, 1852, 1857, 1862, 1867, 1872, 1877, 1882, 1887, 1892, 1897, 1902, 1907, 1912, 1917, 1922, 1927, 1932, 1937, 1942, 1947, 1952, 1957, 1962, 1967, 1972, 1977, 1982, 1987, 1992, 1997, 2002, 2007, 2012, 2017, 2022, 2027, 2032, 2037, 2042, 2047, 2052, 2057, 2062, 2067, 2072, 2077, 2082, 2087, 2092, 2097, 2102, 2107, 2112, 2117, 2122, 2127, 2132, 2137, 2142, 2147, 2152, 2157, 2162, 2167, 2172, 2177, 2182, 2187, 2192, 2197, 2202, 2207, 2212, 2217, 2222, 2227, 2232, 2237, 2242, 2247, 2252, 2257, 2262, 2267, 2272, 2277, 2282, 2287, 2292, 2297, 2302, 2307, 2312, 2317, 2322, 2327, 2332, 2337, 2342, 2347, 2352, 2357, 2362, 2367, 2372, 2377, 2382, 2387, 2392, 2397, 2402, 2407, 2412, 2417, 2422, 2427, 2432, 2437, 2442, 2447, 2452, 2457, 2462, 2467, 2472, 2477, 2482, 2487, 2492, 2497, 2502, 2507, 2512, 2517, 2522, 2527, 2532, 2537, 2542, 2547, 2552, 2557, 2562, 2567, 2572, 2577, 2582, 2587, 2592, 2597, 2602, 2607, 2612, 2617, 2622, 2627, 2632, 2637, 2642, 2647, 2652, 2657, 2662, 2667, 2672, 2677, 2682, 2687, 2692, 2697, 2702, 2707, 2712, 2717, 2722, 2727, 2732, 2737, 2748, 2756, 2764, 2772, 2780, 2788, 2796, 2804, 2812, 2820, 2828, 2836, 2844, 2852, 2860, 2868, 2876, 2884, 2892, 2900, 2908, 2916, 2924, 2932, 2940, 2948, 2956, 2964, 2972, 2980, 2988, 3309, 3314, 3319, 3324, 3329, 3334, 3339, 3344, 3349, 3354, 3359, 3364, 3369, 3374, 3379, 3384, 3389, 3394, 3399, 3404, 3409, 3414, 3419, 3424, 3429, 3434, 3439, 3444, 3449, 3454, 3459, 3464, 3469, 3474, 3479, 3484, 3489, 3494, 3499, 3504, 3509, 3514, 3519, 3524, 3529, 3534, 3539, 3544, 3549, 3554, 3559, 3564, 3569, 3574, 3579, 3584, 3589, 3594, 3599, 3604, 3609, 3614, 3619, 3624, 3629, 3634, 3645, 3653, 3661, 3669, 3677, 3685, 3693, 3701, 3709, 3717, 3725, 3733, 3741, 3749, 3757, 3765]
SAVE_TEX = []
RAW_TEX = []
def rid(r):
    try: return int(str(r).replace('ResourceId::', ''))
    except Exception: return str(r)
def var_to_py(v):
    if len(v.members): return {m.name: var_to_py(m) for m in v.members}
    n = v.rows * v.columns
    if v.type in (rd.VarType.UInt, rd.VarType.Bool): vals = list(v.value.u32v[:n])
    elif v.type == rd.VarType.SInt: vals = list(v.value.s32v[:n])
    else: vals = list(v.value.f32v[:n])
    return vals[0] if n == 1 else vals
def main(controller):
    texs = {rid(t.resourceId): t for t in controller.GetTextures()}
    out = {}
    for eid in EIDS:
        controller.SetFrameEvent(eid, True)
        pipe = controller.GetPipelineState()
        rec = {}
        for stage in (rd.ShaderStage.Vertex, rd.ShaderStage.Fragment):
            refl = pipe.GetShaderReflection(stage)
            if refl is None or stage == rd.ShaderStage.Vertex: continue
            used = pipe.GetConstantBlocks(stage)
            for i, cb in enumerate(refl.constantBlocks):
                desc = next((u.descriptor for u in used if u.access.index == i), None)
                if desc is None: continue
                try:
                    vars_ = controller.GetCBufferVariableContents(pipe.GetGraphicsPipelineObject(), pipe.GetShader(stage), stage, pipe.GetShaderEntryPoint(stage), i, desc.resource, desc.byteOffset, desc.byteSize)
                    rec[f'{int(stage)}:{cb.name}'] = {v.name: var_to_py(v) for v in vars_ if not v.name.startswith('M_') or v.name in ('M_view','M_projection')}
                except Exception as e: rec[f'{int(stage)}:{cb.name}'] = str(e)
            rec[f'tex{int(stage)}'] = {f'{u.access.index}[{u.access.arrayElement}]': rid(u.descriptor.resource) for u in pipe.GetReadOnlyResources(stage) if rid(u.descriptor.resource)}
        try:
            rec['vertexInputs'] = [{'name': a.name, 'buf': a.vertexBuffer, 'off': a.byteOffset, 'fmt': a.format.Name(), 'inst': a.perInstance} for a in pipe.GetVertexInputs()]
            rec['vbuffers'] = []
            for i, vb in enumerate(pipe.GetVBuffers()):
                if rid(vb.resourceId) == 0: continue
                data = controller.GetBufferData(vb.resourceId, vb.byteOffset, min(vb.byteSize or 512, 512))
                rec['vbuffers'].append({'slot': i, 'stride': vb.byteStride, 'floats': list(struct.unpack('<%df' % (len(data)//4), data[:(len(data)//4)*4]))[:64]})
            ib = pipe.GetIBuffer()
            data = controller.GetBufferData(ib.resourceId, ib.byteOffset, 24)
            rec['indices'] = list(struct.unpack('<%dH' % (len(data)//2), data))
        except Exception: rec['vb_err'] = traceback.format_exc()
        out[eid] = rec
    json.dump(out, open(os.path.join(OUTDIR, 'lens_draws.json'), 'w'), indent=1, default=str)
    log = []
    SAVE_TEX.extend(sorted({t for r in out.values() for t in r.get('tex4', {}).values() if t in texs and texs[t].width <= 2048 and texs[t].width > 16}))
    for tid in SAVE_TEX:
        t = texs.get(tid)
        if t is None: log.append(f'{tid} missing'); continue
        sd = rd.TextureSave(); sd.resourceId = t.resourceId; sd.destType = rd.FileType.PNG; sd.alpha = rd.AlphaMapping.Preserve
        log.append(f'{tid} {t.width}x{t.height} {t.format.Name()} -> {controller.SaveTexture(sd, os.path.join(OUTDIR, f"tex_{tid}.png"))}')
    for tid in RAW_TEX:
        t = texs.get(tid)
        if t is None: log.append(f'{tid} missing'); continue
        data = controller.GetTextureData(t.resourceId, rd.Subresource(0, 0, 0))
        open(os.path.join(OUTDIR, f'lut_{tid}.raw'), 'wb').write(bytes(data))
        log.append(f'{tid} {t.width}x{t.height}x{t.depth} {t.format.Name()} raw {len(data)}B')
    open(os.path.join(OUTDIR, 'lens_textures.log'), 'w').write('\n'.join(log))
try:
    pyrenderdoc.LoadCapture(CAPTURE, rd.ReplayOptions(), CAPTURE, False, True)
    pyrenderdoc.Replay().BlockInvoke(main)
except Exception:
    open(os.path.join(OUTDIR, 'lens_error.txt'), 'w').write(traceback.format_exc())
os._exit(0)
