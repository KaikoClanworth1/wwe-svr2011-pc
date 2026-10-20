"""HCTP stage/models -> .glb (+ optional self-contained three.js viewer HTML).

    python to_glb.py <out.glb> <texbank 00200.bin> <yobj or dir>... [--html out.html]
                     [--place file.yobj=x,y,z,ry ...]

Y is flipped (PS2 y-down -> glTF y-up), which mirrors, so triangle winding is swapped.
Each model becomes a glTF node named after its file and object name.
"""
import base64
import io
import json
import os
import struct
import sys

import numpy as np
from PIL import Image

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))  # (its own folder: the embeddable Python doesn't add it)
import yobj


def build(models, texbank, out, debug=()):
    bin_ = bytearray()
    views, accs, meshes, nodes, mats, imgs, texs = [], [], [], [], [], [], []
    matidx = {}

    def add_view(data, target=None):
        while len(bin_) % 4:
            bin_.append(0)
        v = dict(buffer=0, byteOffset=len(bin_), byteLength=len(data))
        if target:
            v['target'] = target
        bin_.extend(data)
        views.append(v)
        return len(views) - 1

    def add_acc(arr, typ, comp, target=None, minmax=False):
        a = dict(bufferView=add_view(arr.tobytes(), target), componentType=comp, count=len(arr), type=typ)
        if minmax:
            a['min'] = arr.min(0).tolist()
            a['max'] = arr.max(0).tolist()
        accs.append(a)
        return len(accs) - 1

    def material(tname):
        if tname in matidx:
            return matidx[tname]
        img = texbank.get(tname)
        m = dict(name=tname, pbrMetallicRoughness=dict(metallicFactor=0, roughnessFactor=1), doubleSided=True)
        if img is not None:
            buf = io.BytesIO()
            Image.fromarray(img).save(buf, 'PNG')
            imgs.append(dict(bufferView=add_view(buf.getvalue()), mimeType='image/png', name=tname))
            texs.append(dict(source=len(imgs) - 1, sampler=0))
            m['pbrMetallicRoughness']['baseColorTexture'] = dict(index=len(texs) - 1)
            a = img[..., 3]
            if (a < 250).any():
                m['alphaMode'] = 'MASK' if ((a > 5) & (a < 250)).mean() < 0.05 else 'BLEND'
        mats.append(m)
        matidx[tname] = len(mats) - 1
        return matidx[tname]

    root_children = []
    for label, M, place in models:
        prims = []
        for mesh in M.meshes:
            P, N = mesh['pos'], mesh['nrm']
            for sub in mesh['subs']:
                pos, nrm, uv, col, idx = [], [], [], [], []
                for st in sub['strips']:
                    base = len(pos)
                    for (vi, stq, c) in st:
                        vi &= 0xFFFF
                        p = P[vi] if vi < len(P) else (0, 0, 0)
                        n = N[vi] if vi < len(N) else (0, 1, 0)
                        pos.append((p[0], -p[1], p[2]))
                        nrm.append((n[0], -n[1], n[2]))
                        q = stq[2] or 1.0
                        uv.append((stq[0] / q, stq[1] / q))
                        col.append([min(1.0, x * 2) for x in c[:3]] + [min(1.0, c[3] * 2)])
                    for a, b, c in yobj.strip_tris(len(st)):
                        idx.append((base + b, base + a, base + c))   # mirrored by the y flip
                if not idx:
                    continue
                tname = M.textures[sub['tex']] if 0 <= sub['tex'] < len(M.textures) else None
                prims.append(dict(
                    attributes=dict(
                        POSITION=add_acc(np.array(pos, np.float32), 'VEC3', 5126, 34962, True),
                        NORMAL=add_acc(np.array(nrm, np.float32), 'VEC3', 5126, 34962),
                        TEXCOORD_0=add_acc(np.array(uv, np.float32), 'VEC2', 5126, 34962),
                        COLOR_0=add_acc(np.array(col, np.float32), 'VEC4', 5126, 34962)),
                    indices=add_acc(np.array(idx, np.uint32).ravel(), 'SCALAR', 5125, 34963),
                    material=material(tname)))
        if not prims:
            continue
        meshes.append(dict(name=label, primitives=prims))
        node = dict(name=f'{label} {M.name}', mesh=len(meshes) - 1)
        if place:
            x, y, z, ry = place
            node['translation'] = [x, -y, z]
            import math
            node['rotation'] = [0, math.sin(math.radians(ry) / 2), 0, math.cos(math.radians(ry) / 2)]
        nodes.append(node)
        root_children.append(len(nodes) - 1)
    for label, T, rgba in debug:
        T = np.asarray(T, np.float32).reshape(-1, 3).copy()
        T[:, 1] *= -1
        idx = np.arange(len(T), dtype=np.uint32).reshape(-1, 3)[:, [1, 0, 2]].ravel()
        mats.append(dict(name=label, pbrMetallicRoughness=dict(baseColorFactor=list(rgba), metallicFactor=0, roughnessFactor=1),
                         doubleSided=True, alphaMode='BLEND' if rgba[3] < 1 else 'OPAQUE'))
        meshes.append(dict(name=label, primitives=[dict(attributes=dict(POSITION=add_acc(T, 'VEC3', 5126, 34962, True)),
                                                        indices=add_acc(idx, 'SCALAR', 5125, 34963), material=len(mats) - 1)]))
        nodes.append(dict(name=label, mesh=len(meshes) - 1))
        root_children.append(len(nodes) - 1)
    gltf = dict(asset=dict(version='2.0', generator='hctp to_glb'), scene=0,
                scenes=[dict(nodes=root_children)], nodes=nodes, meshes=meshes,
                accessors=accs, bufferViews=views, materials=mats,
                buffers=[dict(byteLength=len(bin_))])
    if imgs:
        gltf.update(images=imgs, textures=texs, samplers=[dict(wrapS=10497, wrapT=10497)])
    js = json.dumps(gltf).encode()
    js += b' ' * (-len(js) % 4)
    while len(bin_) % 4:
        bin_.append(0)
    glb = struct.pack('<III', 0x46546C67, 2, 12 + 8 + len(js) + 8 + len(bin_))
    glb += struct.pack('<II', len(js), 0x4E4F534A) + js + struct.pack('<II', len(bin_), 0x004E4942) + bin_
    open(out, 'wb').write(glb)
    return glb


VIEWER = r"""<!doctype html><html><head><meta charset="utf-8"><title>HCTP Map Viewer</title>
<style>body{margin:0;background:#111;color:#ddd;font:13px sans-serif;overflow:hidden}
#ui{position:absolute;top:8px;left:8px;background:#000a;padding:8px;max-height:90vh;overflow:auto;width:260px}
#ui label{display:block;white-space:nowrap}</style></head><body><div id="ui"><b>%TITLE%</b><br>
WASD/QE fly, drag look, wheel speed, F = frame all<div id="list"></div><div id="info"></div></div>
<script type="importmap">{"imports":{"three":"https://cdn.jsdelivr.net/npm/three@0.160.0/build/three.module.js","three/addons/":"https://cdn.jsdelivr.net/npm/three@0.160.0/examples/jsm/"}}</script>
<script type="module">
import * as THREE from 'three';
import {GLTFLoader} from 'three/addons/loaders/GLTFLoader.js';
const b64="%GLB%";
const bytes=Uint8Array.from(atob(b64),c=>c.charCodeAt(0));
const r=new THREE.WebGLRenderer({antialias:true});r.setSize(innerWidth,innerHeight);document.body.appendChild(r.domElement);
const sc=new THREE.Scene();sc.background=new THREE.Color(0x202028);
sc.add(new THREE.AmbientLight(0xffffff,2.2));
const cam=new THREE.PerspectiveCamera(60,innerWidth/innerHeight,0.5,20000);
let yaw=0,pitch=-0.3,speed=60;const keys={};
addEventListener('keydown',e=>{keys[e.key.toLowerCase()]=1;if(e.key=='f')frame();});addEventListener('keyup',e=>keys[e.key.toLowerCase()]=0);
let drag=null;r.domElement.onmousedown=e=>drag=[e.clientX,e.clientY];onmouseup=()=>drag=null;
onmousemove=e=>{if(!drag)return;yaw-=(e.clientX-drag[0])*0.004;pitch-=(e.clientY-drag[1])*0.004;drag=[e.clientX,e.clientY];};
r.domElement.onwheel=e=>{speed*=e.deltaY>0?0.8:1.25;};
let root;
function frame(){const bb=new THREE.Box3().setFromObject(root);const c=bb.getCenter(new THREE.Vector3());const s=bb.getSize(new THREE.Vector3()).length();
cam.position.set(c.x,c.y+s*0.4,c.z+s*0.6);yaw=0;pitch=-0.6;}
new GLTFLoader().parse(bytes.buffer,'',g=>{root=g.scene;sc.add(root);
root.traverse(o=>{if(o.isMesh){o.material.vertexColors=true;o.material.side=THREE.DoubleSide;}});
const list=document.getElementById('list');
root.children.forEach(o=>{const l=document.createElement('label');const c=document.createElement('input');c.type='checkbox';c.checked=true;
c.onchange=()=>o.visible=c.checked;l.appendChild(c);l.append(' '+o.name);list.appendChild(l);});
frame();
window.setCam=(x,y,z,yw,pt)=>{cam.position.set(x,y,z);yaw=yw;pitch=pt;};
window.show=(re,on)=>root.children.forEach(o=>{if(new RegExp(re).test(o.name))o.visible=on;});
window.ready=true;});
const clock=new THREE.Clock();
(function loop(){requestAnimationFrame(loop);const dt=clock.getDelta();
cam.rotation.set(pitch,yaw,0,'YXZ');const f=new THREE.Vector3(0,0,-1).applyEuler(cam.rotation),rt=new THREE.Vector3(1,0,0).applyEuler(cam.rotation);
if(keys.w)cam.position.addScaledVector(f,speed*dt);if(keys.s)cam.position.addScaledVector(f,-speed*dt);
if(keys.d)cam.position.addScaledVector(rt,speed*dt);if(keys.a)cam.position.addScaledVector(rt,-speed*dt);
if(keys.e)cam.position.y+=speed*dt;if(keys.q)cam.position.y-=speed*dt;
document.getElementById('info').textContent='cam '+cam.position.toArray().map(v=>v.toFixed(1)).join(', ')+' (HCTP y = -y)';
r.render(sc,cam);})();
onresize=()=>{cam.aspect=innerWidth/innerHeight;cam.updateProjectionMatrix();r.setSize(innerWidth,innerHeight);};
</script></body></html>"""


def main(argv):
    out, tbf = argv[0], argv[1]
    html = None
    places = {}
    files = []
    i = 2
    while i < len(argv):
        a = argv[i]
        if a == '--html':
            html = argv[i + 1]; i += 2; continue
        if a == '--place':
            k, v = argv[i + 1].split('=')
            places[k] = tuple(float(x) for x in v.split(','))
            i += 2; continue
        if os.path.isdir(a):
            files += sorted(os.path.join(a, f) for f in os.listdir(a) if f.endswith('.yobj'))
        else:
            files.append(a)
        i += 1
    tb = {}
    for t in tbf.split(';'):
        tb.update(yobj.texbank(open(t, 'rb').read()))
    models = []
    for f in files:
        try:
            M = yobj.read(open(f, 'rb').read())
        except Exception as e:  # noqa
            print('skip', f, e)
            continue
        models.append((os.path.splitext(os.path.basename(f))[0], M, places.get(os.path.basename(f))))
    glb = build(models, tb, out)
    print(out, len(glb), 'bytes', len(models), 'models')
    if html:
        open(html, 'w').write(VIEWER.replace('%GLB%', base64.b64encode(glb).decode())
                              .replace('%TITLE%', os.path.basename(out)))


if __name__ == '__main__':
    main(sys.argv[1:])
