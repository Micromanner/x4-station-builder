"""Extract + decompile every shader stage of the given pipelines straight from the
RenderDoc XML+zip export (module id -> zip buffer index), so no module is dumped twice."""
import sys, zipfile, subprocess, os
import xml.etree.ElementTree as ET

xml_path, zip_path = sys.argv[1], sys.argv[2]
targets = set(sys.argv[3:])
out_dir = 'tools/extracted_spv/pipes'
os.makedirs(out_dir, exist_ok=True)
CROSS = r'C:\Program Files\RenderDoc\plugins\spirv\spirv-cross.exe'

mod_to_buf, pipe_to_stages = {}, {}
for _, elem in ET.iterparse(xml_path, events=('end',)):
    name = elem.attrib.get('name')
    if elem.tag != 'chunk':
        continue
    if name == 'vkCreateShaderModule':
        buf = elem.find('.//buffer')
        mid = next((r.text for r in elem.findall('.//ResourceId') if r.attrib.get('name') == 'ShaderModule'), None)
        if buf is not None and mid:
            mod_to_buf[mid] = int(buf.text)
    elif name in ('vkCreateGraphicsPipelines', 'vkCreateComputePipelines'):
        pid = next((r.text for r in elem.findall('.//ResourceId') if r.attrib.get('name') == 'Pipeline'), None)
        if pid:
            stages = {}
            for s in elem.findall('.//struct'):
                if s.attrib.get('typename') == 'VkPipelineShaderStageCreateInfo':
                    st = s.find('.//enum[@typename="VkShaderStageFlagBits"]')
                    mod = next((r.text for r in s.findall('.//ResourceId') if r.attrib.get('typename') == 'VkShaderModule'), None)
                    stages[st.attrib.get('string', '?') if st is not None else '?'] = mod
            pipe_to_stages[pid] = stages
    elem.clear()

z = zipfile.ZipFile(zip_path)
for pid in sorted(targets, key=int):
    stages = pipe_to_stages.get(pid)
    if not stages:
        print(f'pipeline {pid}: NOT FOUND'); continue
    for st, mod in stages.items():
        stage = 'frag' if 'FRAGMENT' in st else 'vert' if 'VERTEX' in st else 'comp'
        buf = mod_to_buf.get(mod)
        if buf is None:
            print(f'pipeline {pid} {stage}: module {mod} has no buffer'); continue
        spv = z.read(f'{buf:06d}')
        spv_path = f'{out_dir}/p{pid}_{stage}_m{mod}.spv'
        open(spv_path, 'wb').write(spv)
        res = subprocess.run([CROSS, spv_path, '--entry', 'main', '--stage', stage, '--vulkan-semantics'], capture_output=True, text=True)
        glsl = f'{out_dir}/p{pid}_{stage}_m{mod}.glsl'
        if res.returncode == 0:
            open(glsl, 'w', encoding='utf-8').write(res.stdout)
            print(f'pipeline {pid} {stage}: module {mod} buf {buf} {len(spv)}B -> {glsl}')
        else:
            print(f'pipeline {pid} {stage}: spirv-cross failed: {res.stderr[:120]}')
