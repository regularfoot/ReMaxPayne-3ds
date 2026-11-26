"""Prepare streamable LCD frames and synchronised NDSP sound from the disc movies."""
import argparse
import ctypes
import ctypes.util
from pathlib import Path
import struct
import subprocess
import sys
from tempfile import TemporaryDirectory

import numpy as np


def pack(source, output):
    library=ctypes.util.find_library('lz4')
    if not library:
        location=(Path(subprocess.check_output(['brew','--prefix','lz4'],text=True).strip())/'lib' if sys.platform=='darwin'
                  else Path(subprocess.check_output(['pkg-config','--variable=libdir','liblz4'],text=True).strip()))
        library=str(Path(location)/('liblz4.dylib' if sys.platform=='darwin' else 'liblz4.so'))
    codec = ctypes.CDLL(library)
    codec.LZ4_compress_default.argtypes = [ctypes.c_void_p,ctypes.c_void_p,ctypes.c_int,ctypes.c_int]
    rate = 15
    frame_size = 400*240*2
    compressed = ctypes.create_string_buffer(codec.LZ4_compressBound(frame_size))
    output.parent.mkdir(parents=True, exist_ok=True)
    with TemporaryDirectory() as directory:
        pcm = Path(directory)/'audio.pcm'
        subprocess.run(['ffmpeg','-nostdin','-v','error','-i',source,'-vn','-ar','22050','-ac','1','-f','s16le',pcm],check=True)
        cmd = ['ffmpeg','-nostdin','-v','error','-i',source,'-an','-vf',
               'fps=15,scale=400:240:flags=lanczos,setsar=1',
               '-pix_fmt','rgb565le','-f','rawvideo','pipe:1']
        temp = output.with_suffix('.partial')
        with subprocess.Popen(cmd,stdout=subprocess.PIPE) as video,pcm.open('rb') as audio,temp.open('wb') as dst:
            dst.write(struct.pack('<4sII',b'MPV1',rate,0))
            count=0
            while frame := video.stdout.read(frame_size):
                if len(frame)!=frame_size:raise ValueError('Truncated movie')
                rotated=np.frombuffer(frame,'<u2').reshape(240,400)[::-1].T.tobytes()
                n=codec.LZ4_compress_default(rotated,compressed,frame_size,len(compressed))
                if not n:raise ValueError('Could not compress frame')
                dst.write(struct.pack('<I',n))
                dst.write(compressed.raw[:n])
                dst.write(audio.read(2*22050//rate).ljust(2*22050//rate,b'\0'))
                count+=1
            if video.wait()!=0 or not count:raise ValueError(source)
            dst.seek(8);dst.write(struct.pack('<I',count))
        temp.replace(output)


if __name__=='__main__':
    parser=argparse.ArgumentParser(description=__doc__)
    parser.add_argument('disc',type=Path)
    parser.add_argument('out',type=Path)
    args=parser.parse_args()
    for name in ('intro','credits','attract00','attract01'):
        pack(args.disc/'english/60'/(name+'.bik'),args.out/(name+'.mpv'))
