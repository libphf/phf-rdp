/**
 * PHF-RDP WebAssembly SIMD Decoder Worker
 * Decompresses full keyframes and delta sub-frames in dedicated background threads.
 */

importScripts('phf.js?v=30');

let mod = null;
let srcPtr = 0, srcCap = 0;
let dstPtr = 0, dstCap = 0;

async function init() {
  if (!mod && typeof PHFModule === 'function') {
    mod = await PHFModule({
      locateFile: (p) => `${p}?v=30`
    });
  }
}

self.onmessage = async (e) => {
  const data = e.data;
  if (data.type === 'init') {
    await init();
    self.postMessage({ type: 'ready' });
    return;
  }

  if (!mod) await init();

  const { frameId, opcode, u8, width, height, encMs, rx, ry, fullW, fullH } = data;
  const phfLen = u8.byteLength;

  if (srcCap < phfLen) {
    if (srcPtr) mod._free(srcPtr);
    srcCap = Math.max(phfLen * 2, 65536);
    srcPtr = mod._malloc(srcCap);
    if (!srcPtr) {
      srcCap = 0;
      self.postMessage({ frameId, error: 'OOM_SRC' });
      return;
    }
  }
  mod.HEAPU8.set(u8, srcPtr);

  const outBytes = width * height * 4;
  if (dstCap < outBytes) {
    if (dstPtr) mod._free(dstPtr);
    dstCap = Math.max(outBytes * 2, 1048576);
    dstPtr = mod._malloc(dstCap);
    if (!dstPtr) {
      dstCap = 0;
      self.postMessage({ frameId, error: 'OOM_DST' });
      return;
    }
  }

  const t0 = performance.now();
  const rc = mod._phf_decode_memory(srcPtr, phfLen, dstPtr, width, height, 4);
  const decMs = performance.now() - t0;

  if (rc === 0) {
    const outBuf = new Uint8Array(outBytes);
    outBuf.set(new Uint8Array(mod.HEAPU8.buffer, dstPtr, outBytes));

    self.postMessage({
      frameId,
      opcode,
      width,
      height,
      encMs,
      decMs,
      rx,
      ry,
      fullW,
      fullH,
      phfLen,
      buffer: outBuf.buffer
    }, [outBuf.buffer]);
  } else {
    self.postMessage({ frameId, error: rc });
  }
};
