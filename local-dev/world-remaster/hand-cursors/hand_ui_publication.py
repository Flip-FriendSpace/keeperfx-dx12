# SPDX-License-Identifier: GPL-2.0-or-later
# KeeperFX hand/cursor reconstruction changes: 2026-10-16.
"""Replay generated RGBA over verified local native sources; never accesses runtime.
Native smoothing, coverage, palettes, fallback and aliases are reconstructed.
"""
import hashlib
import struct
LIMIT = 64 * 1024 * 1024
HEADER = struct.Struct('<4sHHI')
RECORD = struct.Struct('<IHHHHBBHQII')

def require(value, message):
    if not value: raise ValueError(message)

def digest(data): return hashlib.sha256(data).hexdigest()

def preflight():
    import PIL, numpy
    require((PIL.__version__, numpy.__version__) == ('12.3.0', '2.3.5'),
            'Hand UI replay requires Pillow==12.3.0 and numpy==2.3.5')

def fnv64(data):
    value = 14695981039346656037
    for byte in data: value = ((value ^ byte) * 1099511628211) & 0xffffffffffffffff
    return value

def decode(data, width, height):
    import numpy as np
    require(0 < width <= 1024 and 0 < height <= 1024, 'Invalid native dimensions')
    indices = np.zeros((height, width), np.uint8); coverage = np.zeros_like(indices); at = 0
    for y in range(height):
        x = 0
        while True:
            require(at < len(data), 'Truncated native RLE'); command = data[at]; at += 1
            if not command: break
            count = command if command < 128 else 256 - command
            require(x + count <= width, 'Native row overflow')
            if command < 128:
                require(at + count <= len(data), 'Truncated native literals')
                indices[y, x:x+count] = np.frombuffer(data[at:at+count], np.uint8)
                coverage[y, x:x+count] = 255; at += count
            x += count
    return indices, coverage, at

def encode(indices, coverage):
    height, width = coverage.shape; output = bytearray()
    for y in range(height):
        x = 0
        while x < width:
            opaque = coverage[y,x] >= 64; end = x+1
            while end < width and end-x < 127 and (coverage[y,end] >= 64) == opaque: end += 1
            output.append(end-x if opaque else 256-(end-x))
            if opaque: output.extend(indices[y,x:end].tobytes())
            x = end
        output.append(0)
    return bytes(output)

def pixel_digest(indices, coverage):
    import numpy as np
    return digest(np.where(coverage > 0, indices, 0).astype(np.uint8).tobytes() + coverage.tobytes())

class Replay:
    def __init__(self, recipe, sources, load_object):
        import numpy as np
        from PIL import Image
        self.np, self.Image = np, Image
        require(recipe.get('schema') == 1 and recipe.get('algorithm') == 'hand-ui-v8', 'Recipe version')
        require(0 < len(recipe['sources']) <= 16 and 0 < len(recipe['sequences']) <= 512
                and 0 < len(recipe['outputs']) <= 4096, 'Recipe limits')
        self.recipe, self.sources, self.load_object = recipe, sources, load_object
        for name, entry in recipe['sources'].items():
            require(isinstance(name,str) and ':' not in name and '\\' not in name
                    and all(p not in ('','.','..') for p in name.split('/')), 'Unsafe native key')
            require(name in sources, 'Missing verified native source: ' + name)
            raw = sources[name]
            require(0 < len(raw) <= LIMIT and len(raw) == entry['decoded_bytes']
                    and digest(raw) == entry['decoded_sha256'], 'Decoded source mismatch: ' + name)
        self.sequences = {s['id']:s for s in recipe['sequences']}
        require(len(self.sequences) == len(recipe['sequences']), 'Duplicate sequence')
        self.native_cache, self.raw_gain_cache, self.gain_cache = {}, {}, {}
        self.weight_cache, self.quant_cache = {}, {}

    def native(self, spec):
        np, Image = self.np, self.Image
        key = (spec['kind'],spec['data'],spec['table'],spec['palette'],spec['index'])
        if key in self.native_cache: return self.native_cache[key]
        for name in key[1:4]: require(name in self.recipe['sources'], 'Unbound native dependency')
        data, table, palette = (self.sources[name] for name in key[1:4]); index = spec['index']
        require(type(index) is int and index >= 0 and len(palette) == 768, 'Native binding')
        if spec['kind'] == 'ui':
            require(len(table)%6 == 0 and index < len(table)//6, 'UI table bounds')
            offset,width,height = struct.unpack_from('<IBB',table,index*6)
            full_width,full_height,x,y = width,height,0,0
        elif spec['kind'] == 'keeper':
            require(len(table)%16 == 0 and index < len(table)//16, 'Keeper table bounds')
            offset,width,height,full_width,full_height,rotation,count,x,y,ax,ay = struct.unpack_from('<I8Bhh',table,index*16)
            require(rotation == 0 and count > 0, 'Pinned nonrotating keeper source required')
        else: raise ValueError('Unknown native kind')
        require(offset < len(data) and 0 < full_width <= 256 and 0 < full_height <= 256
                and x+width <= full_width and y+height <= full_height, 'Native canvas bounds')
        indices,coverage,used = decode(memoryview(data)[offset:],width,height)
        colours = (np.frombuffer(palette,np.uint8).reshape(256,3).astype(np.uint16)*4).astype(np.uint8)
        rgba = np.zeros((height,width,4),np.uint8)
        rgba[:,:,:3][coverage > 0] = colours[indices[coverage > 0]]; rgba[:,:,3] = coverage
        image = Image.fromarray(rgba)
        if spec['kind'] == 'keeper':
            canvas = Image.new('RGBA',(full_width,full_height)); canvas.alpha_composite(image,(x,y)); image = canvas
        result = (image,fnv64(memoryview(data)[offset:offset+used])); self.native_cache[key] = result
        return result

    def raw_gain(self, sequence_id, frame):
        np, Image = self.np, self.Image; key = (sequence_id,frame)
        if key in self.raw_gain_cache: return self.raw_gain_cache[key]
        sequence = self.sequences[sequence_id]
        require(type(frame) is int and 0 <= frame < len(sequence['frames']), 'Gain frame bounds')
        node = sequence['frames'][frame]; original,_ = self.native(node['native'])
        width,height = original.width*4,original.height*4; raw = self.load_object(node['generated_object'])
        require(len(raw) == width*height*4 and len(raw) <= 4*1024*1024
                and digest(raw) == node['generated_object'], 'Generated RGBA mismatch')
        src = np.asarray(original.resize((width,height),Image.Resampling.BILINEAR)).astype('float32')
        generated = np.frombuffer(raw,np.uint8).reshape(height,width,4).astype('float32')
        def lum(a): return .2126*a[:,:,0]+.7152*a[:,:,1]+.0722*a[:,:,2]
        gain = np.clip((lum(generated)+32)/(lum(src)+32),.5,1.7)
        gain = np.where(generated[:,:,3] >= 64,gain,1); self.raw_gain_cache[key] = gain
        return gain

    def weight(self, sequence_id):
        np = self.np
        if sequence_id in self.weight_cache: return self.weight_cache[sequence_id]
        sequence = self.sequences[sequence_id]; sizes = [self.native(n['native'])[0].size for n in sequence['frames']]
        width,height = max(w for w,h in sizes)*4,max(h for w,h in sizes)*4
        y,x = np.mgrid[0:height,0:width]; x,y = (x+.5)/4,(y+.5)/4
        weight = np.zeros((height,width),dtype='float32')
        settings = {'hand0-hover':(x+y,45,63),'hand0-pickup':(x+y,41,59),'hand0-holdgold':(y,16,27),
                    'hand0-sidehover':(x,30,46),'hand0-slap':(x,40,53),'landview-ready':(y,30,46)}
        if sequence_id in settings:
            coordinate,low,high = settings[sequence_id]; t = np.clip((coordinate-low)/(high-low),0,1); weight = t*t*(3-2*t)
        if sequence['kind'] == 'Pointer canonical' and len(sequence['frames']) > 1:
            d = np.minimum.reduce([np.maximum(x,y),np.maximum(width/4-x,y),np.maximum(x,height/4-y),np.maximum(width/4-x,height/4-y)])
            t = np.clip((15-d)/5,0,1); weight = t*t*(3-2*t)
        self.weight_cache[sequence_id] = weight; return weight

    def gain(self, reference):
        np = self.np; sequence_id,frame = reference; key = (sequence_id,frame)
        if key in self.gain_cache: return self.gain_cache[key]
        gain = self.raw_gain(sequence_id,frame); weights = self.weight(sequence_id); q = weights[:gain.shape[0],:gain.shape[1]]
        first = np.ones(weights.shape,dtype='float32'); first_gain = self.raw_gain(sequence_id,0)
        first[:first_gain.shape[0],:first_gain.shape[1]] = first_gain
        result = gain*(1-q)+first[:gain.shape[0],:gain.shape[1]]*q; self.gain_cache[key] = result; return result

    def output_pixels(self, record):
        np,Image = self.np,self.Image; original,fingerprint = self.native(record['native']); gain = self.gain(record['gain'])
        if 'translation' in record:
            dx,dy = record['translation']; require(type(dx) is int and type(dy) is int and abs(dx) <= 256 and abs(dy) <= 256, 'Alias translation bounds')
            target = Image.fromarray(np.ones((original.height*4,original.width*4),dtype='float32'))
            target.paste(Image.fromarray(gain.astype('float32')),(dx*4,dy*4)); gain = np.asarray(target)
        require(gain.shape == (original.height*4,original.width*4), 'Alias canvas mismatch')
        image = original.resize((original.width*4,original.height*4),Image.Resampling.BILINEAR)
        array = np.asarray(image).copy(); rgb = np.asarray(image).astype('float32')[:,:,:3]
        array[:,:,:3] = np.clip(np.rint(rgb*gain[:,:,None]),0,255).astype('uint8')
        coverage = np.asarray(original.resize(image.size,Image.Resampling.NEAREST))[:,:,3].copy()
        palette_bytes = self.sources[record['native']['palette']]
        palette = np.frombuffer(palette_bytes,np.uint8).reshape(256,3).astype('int16')
        table = self.quant_cache.setdefault(digest(palette_bytes),{})
        unique,inverse = np.unique((array[:,:,:3]//4).reshape(-1,3),axis=0,return_inverse=True)
        missing = [tuple(map(int,c)) for c in unique if tuple(map(int,c)) not in table]
        for start in range(0,len(missing),2048):
            colours = np.asarray(missing[start:start+2048],dtype='int16')
            best = np.abs(colours[:,None,:]-palette[None,:,:]).sum(2).argmin(1); best[best == 0] = 1
            table.update((c,int(v)) for c,v in zip(missing[start:start+2048],best))
        indices = np.asarray([table[tuple(map(int,c))] for c in unique],np.uint8)[inverse].reshape(coverage.shape)
        return original.width,original.height,fingerprint,indices,coverage

def reconstruct_hand_bank(bank_manifest, verified_decoded_sources, load_object):
    """Return sparse component KFUI; the caller bounds zlib and merges other owners."""
    preflight(); replay = Replay(bank_manifest,verified_decoded_sources,load_object)
    records = bank_manifest['outputs']; output = bytearray(HEADER.pack(b'KFUI',1,32,len(records))); previous = -1
    for record in records:
        index = record['index']; require(type(index) is int and previous < index < 4096,'Unsorted output index'); previous = index
        width,height,fingerprint,indices,coverage = replay.output_pixels(record)
        require(record['native']['kind'] == 'ui' and record['native']['index'] == index,'Output source index mismatch')
        require(pixel_digest(indices,coverage) == record['pixel_sha256'],'Hand UI pixel mismatch')
        data = encode(indices,coverage)
        output.extend(RECORD.pack(index,width,height,width*4,height*4,4,0,0,fingerprint,len(data),0)); output.extend(data)
    require(len(output) <= LIMIT and digest(output) == bank_manifest['sha256'],'Hand UI component hash mismatch')
    return bytes(output)
