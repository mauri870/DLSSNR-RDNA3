#!/usr/bin/env python3
"""Generate pNext sizes from the pinned Vulkan registry, not guessed ABI sizes.

Only core-header declarations are included. Unknown/platform structures cause
the layer to leave device creation untouched and report NR unavailable.
"""
import argparse
import hashlib
import re
import xml.etree.ElementTree as ET
from pathlib import Path

p = argparse.ArgumentParser(description=__doc__)
p.add_argument('headers', type=Path)
p.add_argument('output', type=Path)
a = p.parse_args()
xml = a.headers / 'registry/vk.xml'
root = ET.parse(xml).getroot()
core = (a.headers / 'include/vulkan/vulkan_core.h').read_text()
defined = set(re.findall(r'typedef struct (\w+) \{', core))
lines = ['// Generated from vk.xml SHA256 ' + hashlib.sha256(xml.read_bytes()).hexdigest(),
         '#pragma once', 'inline size_t nr_device_chain_size(VkStructureType type) {',
         '    switch (type) {',
         '    case VK_STRUCTURE_TYPE_LOADER_DEVICE_CREATE_INFO: return sizeof(VkLayerDeviceCreateInfo);']
seen = set()
for t in root.findall('types/type'):
    name = t.get('name')
    if t.get('category') != 'struct' or t.get('alias') or name not in defined:
        continue
    if not {'VkDeviceCreateInfo', 'VkPhysicalDeviceFeatures2'}.intersection(t.get('structextends', '').split(',')):
        continue
    member = t.find("member[@values]")
    if member is None:
        continue
    st = member.get('values')
    if st in seen:
        raise ValueError('duplicate sType ' + st)
    seen.add(st)
    lines.append(f'    case {st}: return sizeof({name});')
lines += ['    default: return 0;', '    }', '}', '']
a.output.write_text('\n'.join(lines))
