//! H.264 → NV12 on the GPU through Direct3D 12 video decoding, with FFmpeg's decoder in
//! `plugcam_dec.dll` (built from `decoder/` by `decoder/build.sh`).
//!
//! The decoder built into Windows keeps two of its own threads busy for as long as the GPU
//! decodes each picture: about 330 ms of processor time per second for 1080p30, against about
//! 30 ms here. A crop is done on the GPU as well, before the picture is read back. The DLL is
//! loaded at runtime; without it, or without Direct3D 12 video decoding, `decode::H264Decoder`
//! uses the Windows decoder. See docs/decoder.md.

use std::ffi::{CStr, c_char, c_void};
use std::path::Path;

use libloading::Library;

use crate::frame::{ColorInfo, Nv12Frame, Prescale};

/// `PC_DEC_API_VERSION` in `decoder/plugcam_dec.c`.
const API_VERSION: i32 = 2;

#[repr(C)]
#[derive(Default)]
struct PcFrame {
    width: i32,
    height: i32,
    y: *const u8,
    y_stride: i32,
    uv: *const u8,
    uv_stride: i32,
    full_range: i32,
    bt709: i32,
    prescaled: i32,
}

type ApiVersion = unsafe extern "C" fn() -> i32;
type Version = unsafe extern "C" fn() -> *const c_char;
type LastError = unsafe extern "C" fn() -> *const c_char;
type Open = unsafe extern "C" fn() -> *mut c_void;
type Close = unsafe extern "C" fn(*mut c_void);
type Send = unsafe extern "C" fn(*mut c_void, *const u8, i32, i64) -> i32;
type Receive = unsafe extern "C" fn(*mut c_void, *mut PcFrame) -> i32;
type SetCrop = unsafe extern "C" fn(*mut c_void, i32, i32, i32, i32, i32, i32) -> i32;

pub struct FfDecoder {
    handle: *mut c_void,
    close: Close,
    send: Send,
    receive: Receive,
    set_crop: SetCrop,
    last_error: LastError,
    size: Option<(u32, u32)>,
    /// The crop last handed to the DLL.
    prescale: Option<Prescale>,
    /// The GPU could not crop; the frame converter does it.
    crop_failed: bool,
    /// SPS/PPS waiting for the next frame: FFmpeg wants them in the same packet (as scrcpy's
    /// own client merges them).
    config: Vec<u8>,
    // Keeps the function pointers above valid; must outlive `handle`.
    _lib: Library,
}

// Used from one thread at a time; FFmpeg's decoder has no thread affinity.
unsafe impl std::marker::Send for FfDecoder {}

fn c_str(p: *const c_char) -> String {
    if p.is_null() { String::new() } else { unsafe { CStr::from_ptr(p) }.to_string_lossy().into_owned() }
}

impl FfDecoder {
    /// Loads the DLL and opens a decoder on the default GPU.
    pub fn open(dll: &Path) -> Result<Self, String> {
        let load_err = |e: libloading::Error| format!("{}: {e}", dll.display());
        unsafe {
            let lib = Library::new(dll).map_err(load_err)?;
            let api_version = *lib.get::<ApiVersion>(b"pc_dec_api_version\0").map_err(load_err)?;
            if api_version() != API_VERSION {
                return Err(format!("{}: API version {}, expected {API_VERSION}", dll.display(), api_version()));
            }
            let version = *lib.get::<Version>(b"pc_dec_version\0").map_err(load_err)?;
            let last_error = *lib.get::<LastError>(b"pc_dec_last_error\0").map_err(load_err)?;
            let open = *lib.get::<Open>(b"pc_dec_open\0").map_err(load_err)?;
            let close = *lib.get::<Close>(b"pc_dec_close\0").map_err(load_err)?;
            let send = *lib.get::<Send>(b"pc_dec_send\0").map_err(load_err)?;
            let receive = *lib.get::<Receive>(b"pc_dec_receive\0").map_err(load_err)?;
            let set_crop = *lib.get::<SetCrop>(b"pc_dec_set_crop\0").map_err(load_err)?;

            let handle = open();
            if handle.is_null() {
                return Err(c_str(last_error()));
            }
            log::info!("decoding on the GPU with FFmpeg {} (Direct3D 12)", c_str(version()));
            Ok(Self {
                handle,
                close,
                send,
                receive,
                set_crop,
                last_error,
                size: None,
                prescale: None,
                crop_failed: false,
                config: Vec::new(),
                _lib: lib,
            })
        }
    }

    pub fn picture_size(&self) -> Option<(u32, u32)> {
        self.size
    }

    /// Crop and scale the next pictures on the GPU before they are read back; such pictures come
    /// out with `prescaled` set. When the GPU cannot, they come out whole and the frame converter
    /// crops them.
    pub fn set_prescale(&mut self, prescale: Option<Prescale>) {
        if prescale == self.prescale {
            return;
        }
        self.prescale = prescale;
        let p = prescale.unwrap_or(Prescale { x: 0, y: 0, width: 0, height: 0, out_width: 0, out_height: 0 });
        let n = |v: u32| v.min(i32::MAX as u32) as i32;
        unsafe { (self.set_crop)(self.handle, n(p.x), n(p.y), n(p.width), n(p.height), n(p.out_width), n(p.out_height)) };
    }

    fn error(&self, what: &str, code: i32) -> String {
        let detail = c_str(unsafe { (self.last_error)() });
        if detail.is_empty() { format!("{what}: error {code}") } else { format!("{what}: {detail}") }
    }

    /// Feeds one Annex B access unit and calls `on_frame` for every picture that comes out.
    pub fn decode(&mut self, data: &[u8], pts_us: u64, on_frame: &mut dyn FnMut(&Nv12Frame)) -> Result<(), String> {
        if is_config(data) {
            self.config = data.to_vec();
            return Ok(());
        }
        let merged;
        let data = if self.config.is_empty() {
            data
        } else {
            let mut config = std::mem::take(&mut self.config);
            config.extend_from_slice(data);
            merged = config;
            &merged[..]
        };
        let len = i32::try_from(data.len()).map_err(|_| "packet too large".to_string())?;
        loop {
            match unsafe { (self.send)(self.handle, data.as_ptr(), len, pts_us as i64) } {
                0 => break,
                // Its queue is full: take pictures out, then send again.
                1 => self.drain(on_frame)?,
                e => return Err(self.error("FFmpeg decoder", e)),
            }
        }
        self.drain(on_frame)
    }

    fn drain(&mut self, on_frame: &mut dyn FnMut(&Nv12Frame)) -> Result<(), String> {
        loop {
            let mut f = PcFrame::default();
            match unsafe { (self.receive)(self.handle, &mut f) } {
                0 => return Ok(()),
                1 => {}
                e => return Err(self.error("FFmpeg decoder", e)),
            }
            let (width, height) = (f.width as u32, f.height as u32);
            let valid = f.width > 0
                && f.height > 0
                && !f.y.is_null()
                && !f.uv.is_null()
                && f.y_stride >= f.width
                && f.uv_stride >= (f.width + 1) / 2 * 2;
            if !valid {
                return Err(format!("FFmpeg decoder: odd picture {}x{} (strides {}, {})", f.width, f.height, f.y_stride, f.uv_stride));
            }
            if self.prescale.is_some() && f.prescaled == 0 && !self.crop_failed {
                log::warn!("GPU crop: {}; cropping on the CPU", c_str(unsafe { (self.last_error)() }));
                self.crop_failed = true;
            }
            if self.size != Some((width, height)) && f.prescaled == 0 {
                log::info!("decoder output: {width}x{height} (stride {}), full range {}, BT.709 {}", f.y_stride, f.full_range != 0, f.bt709 != 0);
                self.size = Some((width, height));
            }
            // FFmpeg's planes are exactly `stride * rows` long (the last row included).
            let y = unsafe { std::slice::from_raw_parts(f.y, f.y_stride as usize * height as usize) };
            let uv = unsafe { std::slice::from_raw_parts(f.uv, f.uv_stride as usize * height.div_ceil(2) as usize) };
            on_frame(&Nv12Frame {
                width,
                height,
                y,
                y_stride: f.y_stride as u32,
                uv,
                uv_stride: f.uv_stride as u32,
                color: ColorInfo { full_range: f.full_range != 0, bt709: f.bt709 != 0 },
                prescaled: f.prescaled != 0,
            });
        }
    }
}

/// Whether an Annex B access unit holds only parameter sets (SPS 7, PPS 8), no picture.
fn is_config(data: &[u8]) -> bool {
    let mut types = data.windows(4).enumerate().filter_map(|(i, w)| match w {
        [0, 0, 1, nal] => Some(nal & 0x1f),
        [0, 0, 0, 1] => data.get(i + 4).map(|nal| nal & 0x1f),
        _ => None,
    });
    let mut any = false;
    types.all(|t| {
        any = true;
        matches!(t, 7 | 8)
    }) && any
}

#[cfg(test)]
mod tests {
    use super::is_config;

    #[test]
    fn tells_parameter_sets_from_pictures() {
        assert!(is_config(&[0, 0, 0, 1, 0x67, 1, 2, 0, 0, 0, 1, 0x68, 3]));
        assert!(!is_config(&[0, 0, 0, 1, 0x67, 1, 0, 0, 1, 0x65, 9, 9]));
        assert!(!is_config(&[0, 0, 0, 1, 0x41, 1, 2, 3]));
        assert!(!is_config(&[1, 2, 3]));
    }
}

impl Drop for FfDecoder {
    fn drop(&mut self) {
        unsafe { (self.close)(self.handle) }
    }
}
