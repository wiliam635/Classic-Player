use nord_format::formats::nsmp::codec;
use nord_format::{from_path, Entity, Sample};
use std::ffi::{c_char, c_void, CStr};
use std::panic::{catch_unwind, AssertUnwindSafe};
use std::path::Path;

type ZoneCallback = unsafe extern "C" fn(
    context: *mut c_void,
    root_key: u8,
    low_note: u8,
    high_note: u8,
    low_velocity: u8,
    high_velocity: u8,
    channels: u16,
    sample_rate: u32,
    gain_db: f32,
    loop_start: u32,
    loop_end: u32,
    samples: *const i16,
    sample_count: usize,
) -> bool;

unsafe fn write_error(buffer: *mut c_char, capacity: usize, message: &str) {
    if buffer.is_null() || capacity == 0 {
        return;
    }
    let bytes = message.as_bytes();
    let count = bytes.len().min(capacity - 1);
    unsafe {
        std::ptr::copy_nonoverlapping(bytes.as_ptr(), buffer.cast::<u8>(), count);
        *buffer.add(count) = 0;
    }
}

fn decode(
    path: &str,
    context: *mut c_void,
    callback: ZoneCallback,
) -> Result<(), String> {
    let entity = from_path(Path::new(path)).map_err(|error| error.to_string())?;
    let Entity::Sample(sample) = entity else {
        return Err("O arquivo não é um sample Nord (.nsmp4).".to_owned());
    };
    if sample.generation() != "v4" {
        return Err("Este importador aceita arquivos Nord Sample 4 (.nsmp4).".to_owned());
    }
    let layout = sample.layout().map_err(|error| error.to_string())?;
    let zones = sample.zones().map_err(|error| error.to_string())?;
    if zones.is_empty() {
        return Err("O sample Nord não contém zonas de teclado.".to_owned());
    }
    let velocity_windows = match &sample {
        Sample::V2(_) => vec![None; zones.len()],
        Sample::V3(body) => body
            .zones()
            .map_err(|error| error.to_string())?
            .into_iter()
            .map(|zone| zone.velocity.map(|window| (window.low, window.high)))
            .collect(),
    };

    let mut ranges = Vec::with_capacity(zones.len());
    for zone in &zones {
        let (low, high) = match zone.low_note {
            Some(low) => (low, zone.top_note),
            None => (0, zone.top_note),
        };
        ranges.push((low, high));
    }
    if zones.iter().any(|zone| zone.low_note.is_none()) {
        let mut order: Vec<usize> = (0..zones.len()).collect();
        order.sort_by_key(|&index| zones[index].top_note);
        for (position, &index) in order.iter().enumerate() {
            let low = if position == 0 {
                0
            } else {
                zones[order[position - 1]].top_note.saturating_add(1)
            };
            ranges[index].0 = low;
        }
    }

    let mut covered = [[false; 128]; 128];
    for (index, zone) in zones.iter().enumerate() {
        let (low, high) = ranges[index];
        let (low_velocity, high_velocity) = velocity_windows[index]
            .unwrap_or((0, 127));
        if low > high || high > 127 || zone.root_key > 127
            || low_velocity > high_velocity || high_velocity > 127
        {
            return Err(format!("A zona {} contém limites de nota ou velocity inválidos.", index + 1));
        }
        for note in low..=high {
            for velocity in low_velocity..=high_velocity {
                let slot = &mut covered[usize::from(note)][usize::from(velocity)];
                if *slot {
                    return Err("O mapa de zonas do sample Nord possui regiões sobrepostas.".to_owned());
                }
                *slot = true;
            }
        }
    }

    let mut decoded_samples = 0usize;
    const MAX_DECODED_PCM_SAMPLES: usize = 64 * 1024 * 1024;
    for (index, zone) in zones.iter().enumerate() {
        let decoded = codec::decode(zone.stream, zone.at, layout)
            .map_err(|error| format!("Não foi possível decodificar a zona {}: {}", index + 1, error))?;
        if decoded.channels == 0 || decoded.channels > 2 || decoded.frames() == 0 {
            return Err(format!("A zona {} possui áudio vazio ou formato inválido.", index + 1));
        }
        if decoded.frames() > u32::MAX as usize {
            return Err(format!("A zona {} excede o limite de frames suportado.", index + 1));
        }
        if decoded.samples.len() > MAX_DECODED_PCM_SAMPLES.saturating_sub(decoded_samples) {
            return Err("O áudio descomprimido excede o limite de memória permitido.".to_owned());
        }
        decoded_samples += decoded.samples.len();
        let loop_start = codec::walk(zone.stream, zone.at, layout)
            .ok()
            .and_then(|stream| stream.records.into_iter().find(|record| record.mark))
            .map(|record| record.first_field)
            .filter(|&frame| frame < decoded.frames())
            .map(|frame| frame.min(u32::MAX as usize) as u32)
            .unwrap_or(u32::MAX);
        let loop_end = if loop_start == u32::MAX {
            0
        } else {
            decoded.frames().min(u32::MAX as usize) as u32
        };
        let gain_db = codec::zone_gain_db(zone.stream, layout)
            .filter(|gain| gain.is_finite())
            .unwrap_or(0.0)
            .clamp(-60.0, 24.0);
        let (low, high) = ranges[index];
        let (low_velocity, high_velocity) = velocity_windows[index]
            .unwrap_or((0, 127));
        let accepted = unsafe {
            callback(
                context,
                zone.root_key,
                low,
                high,
                low_velocity,
                high_velocity,
                decoded.channels,
                codec::FIELD_RATE,
                gain_db,
                loop_start,
                loop_end,
                decoded.samples.as_ptr(),
                decoded.samples.len(),
            )
        };
        if !accepted {
            return Err("Classic Player não conseguiu armazenar as zonas do sample Nord.".to_owned());
        }
    }
    Ok(())
}

#[no_mangle]
pub unsafe extern "C" fn classic_nord_decode_nsmp4(
    path: *const c_char,
    context: *mut c_void,
    callback: Option<ZoneCallback>,
    error_buffer: *mut c_char,
    error_capacity: usize,
) -> i32 {
    let result = catch_unwind(AssertUnwindSafe(|| {
        if path.is_null() {
            return Err("Caminho do arquivo Nord ausente.".to_owned());
        }
        let path = unsafe { CStr::from_ptr(path) }
            .to_str()
            .map_err(|_| "O caminho do arquivo não está em UTF-8.".to_owned())?;
        let callback = callback.ok_or_else(|| "Callback de áudio ausente.".to_owned())?;
        decode(path, context, callback)
    }));

    match result {
        Ok(Ok(())) => 0,
        Ok(Err(message)) => {
            unsafe { write_error(error_buffer, error_capacity, &message) };
            1
        }
        Err(_) => {
            unsafe { write_error(error_buffer, error_capacity, "Falha interna ao decodificar o arquivo Nord.") };
            2
        }
    }
}
