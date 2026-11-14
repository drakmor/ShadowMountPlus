use ampr_pkg::kraken::{decode_block, Desc, Scratch};
use ampr_pkg::{CHUNK_KRAKEN, CHUNK_KRAKEN_FULL};
use std::env;
use std::fs::{self, FileTimes, OpenOptions};
use std::path::Path;
use std::time::{Duration, UNIX_EPOCH};

#[path = "../../../kraken-mount-reference/src/ampr_index.rs"]
#[allow(dead_code)]
mod kraken_ampr_index;

const BLOCK: usize = 0x40000;
const PACKAGE_SIZE: usize = 0x100000;
const EBOOT_OFFSET: usize = 0x20000;
const PARAM_OFFSET: usize = 0x60000;
const STREAM_HEAD_OFFSET: usize = 0x80000;
const STREAM_TAIL_OFFSET: usize = 0xc0000;
const PARAM_SIZE: usize = 257;
const STREAM_TAIL_SIZE: usize = 37;

fn pattern_eboot(index: usize) -> u8 {
    ((index * 29 + index / 113) & 0xff) as u8
}

fn pattern_stream_head(index: usize) -> u8 {
    ((index * 7 + index / 17) & 0xff) as u8
}

fn pattern_stream_tail(index: usize) -> u8 {
    ((index * 13 + index / 41) & 0xff) as u8
}

fn main() {
    let args: Vec<String> = env::args().collect();
    assert_eq!(args.len(), 4, "usage: oracle INDEX MANIFEST PACKAGE");
    let index_bytes = fs::read(&args[1]).unwrap();
    let index = ampr_format::index::Index::open(index_bytes.as_slice())
        .expect("pinned AMPRIDX3 reader accepts C output");
    assert_eq!(index.len(), 3);
    let expected_index = [
        (
            b"/app0/assets/stream.bin".as_slice(),
            (BLOCK + STREAM_TAIL_SIZE) as u64,
        ),
        (b"/app0/eboot.bin".as_slice(), BLOCK as u64),
        (b"/app0/sce_sys/param.json".as_slice(), PARAM_SIZE as u64),
    ];
    for (offset, (expected_path, expected_size)) in expected_index.iter().enumerate() {
        let record = index.record(offset as u32 + 1).unwrap();
        let mut path = vec![0u8; record.path_len as usize];
        assert_eq!(index.path(&record, &mut path).unwrap(), *expected_path);
        assert_eq!(record.size, *expected_size);
        assert_eq!(record.mtime, 123);
    }

    let manifest_bytes = fs::read(&args[2]).unwrap();
    let manifest = ampr_pkg::open_pkg(manifest_bytes.as_slice())
        .expect("pinned AMPRPAK4 pkg reader accepts C output");
    assert_eq!(manifest.file_count(), 3);
    assert_eq!(manifest.chunk_count(), 4);
    assert_eq!(manifest.pack_count(), 1);

    let package = fs::read(&args[3]).unwrap();
    assert_eq!(package.len(), PACKAGE_SIZE);
    let reference_dir = Path::new(&args[2]).parent().unwrap().join("reference");
    fs::create_dir_all(&reference_dir).unwrap();
    let reference_package = reference_dir.join("kraken.pkg");
    fs::copy(&args[3], &reference_package).unwrap();
    OpenOptions::new()
        .write(true)
        .open(&reference_package)
        .unwrap()
        .set_times(FileTimes::new().set_modified(
            UNIX_EPOCH + Duration::from_secs(123),
        ))
        .expect("set deterministic package mtime for AMPR index oracle");
    let reference_files = [
        kraken_ampr_index::IndexFile {
            path: "eboot.bin".to_string(),
            size: BLOCK as u64,
            chunks: Some(vec![kraken_ampr_index::Chunk {
                pkg_off: EBOOT_OFFSET as u64,
                stored_len: BLOCK as u32,
                raw_len: BLOCK as u32,
                kraken: Some(kraken_ampr_index::Kraken {
                    even_len: 0x20000,
                    mode: 0,
                }),
            }]),
        },
        kraken_ampr_index::IndexFile {
            path: "sce_sys/param.json".to_string(),
            size: PARAM_SIZE as u64,
            chunks: Some(vec![kraken_ampr_index::Chunk {
                pkg_off: PARAM_OFFSET as u64,
                stored_len: PARAM_SIZE as u32,
                raw_len: PARAM_SIZE as u32,
                kraken: None,
            }]),
        },
        kraken_ampr_index::IndexFile {
            path: "assets/stream.bin".to_string(),
            size: (BLOCK + STREAM_TAIL_SIZE) as u64,
            chunks: Some(vec![
                kraken_ampr_index::Chunk {
                    pkg_off: STREAM_HEAD_OFFSET as u64,
                    stored_len: BLOCK as u32,
                    raw_len: BLOCK as u32,
                    kraken: None,
                },
                kraken_ampr_index::Chunk {
                    pkg_off: STREAM_TAIL_OFFSET as u64,
                    stored_len: BLOCK as u32,
                    raw_len: BLOCK as u32,
                    kraken: Some(kraken_ampr_index::Kraken {
                        even_len: 0x20000,
                        mode: 0,
                    }),
                },
            ]),
        },
    ];
    kraken_ampr_index::write(&reference_dir, &reference_package, &reference_files)
        .expect("pinned Kraken writer generates reference indexes");
    assert_eq!(
        fs::read(&args[1]).unwrap(),
        fs::read(reference_dir.join("ampr_emu.index")).unwrap(),
        "AMPRIDX3 output matches pinned Kraken writer byte for byte"
    );
    assert_eq!(
        fs::read(&args[2]).unwrap(),
        fs::read(reference_dir.join("ampr_assets.index")).unwrap(),
        "AMPRPAK4 output matches pinned Kraken writer byte for byte"
    );

    let expected_manifest = [
        (
            b"/app0/assets/stream.bin".as_slice(),
            (BLOCK + STREAM_TAIL_SIZE) as u64,
            0,
            2,
        ),
        (b"/app0/eboot.bin".as_slice(), BLOCK as u64, 2, 1),
        (
            b"/app0/sce_sys/param.json".as_slice(),
            PARAM_SIZE as u64,
            3,
            1,
        ),
    ];
    for (id, (expected_path, expected_size, first_chunk, chunk_count)) in
        expected_manifest.iter().enumerate()
    {
        let file = manifest.file(id as u32).unwrap();
        let mut path = vec![0u8; file.path_len as usize];
        assert_eq!(manifest.path(&file, &mut path).unwrap(), *expected_path);
        assert_eq!(file.logical_size, *expected_size);
        assert_eq!(file.mtime, 123);
        assert_eq!(file.first_chunk, *first_chunk);
        assert_eq!(file.chunk_count, *chunk_count);
    }

    let expected_chunks = [
        (STREAM_HEAD_OFFSET, 0, BLOCK as u32, 0),
        (
            STREAM_TAIL_OFFSET,
            CHUNK_KRAKEN,
            BLOCK as u32,
            CHUNK_KRAKEN_FULL,
        ),
        (EBOOT_OFFSET, CHUNK_KRAKEN, BLOCK as u32, 0),
        (PARAM_OFFSET, 0, PARAM_SIZE as u32, 0),
    ];
    let mut scratch = Scratch::new_boxed();
    for (id, (expected_offset, expected_codec, expected_stored, expected_flags)) in
        expected_chunks.iter().enumerate()
    {
        let chunk = manifest.chunk_record(id as u32).unwrap();
        let (offset, pack_id, descriptor) = ampr_pkg::pkg_chunk(chunk);
        assert_eq!(offset, *expected_offset as u64);
        assert_eq!(pack_id, 0);
        assert_eq!(chunk.codec, *expected_codec);
        assert_eq!(chunk.stored_size, *expected_stored);
        assert_eq!(chunk.flags & CHUNK_KRAKEN_FULL, *expected_flags);

        let end = offset as usize + chunk.stored_size as usize;
        let stored = package.get(offset as usize..end).unwrap();
        if chunk.codec == CHUNK_KRAKEN {
            let (even_len, mode) = ampr_pkg::kraken_desc(descriptor);
            let mut decoded = vec![0u8; BLOCK];
            assert_eq!(
                decode_block(stored, Desc { even_len, mode }, &mut decoded, &mut scratch).unwrap(),
                BLOCK
            );
            let expected: Vec<u8> = (0..BLOCK)
                .map(|i| {
                    if *expected_offset == EBOOT_OFFSET {
                        pattern_eboot(i)
                    } else {
                        pattern_stream_tail(i)
                    }
                })
                .collect();
            assert_eq!(decoded, expected, "codec-2 chunk {id} byte readback");
            if *expected_offset == STREAM_TAIL_OFFSET {
                for (index, actual) in decoded[..STREAM_TAIL_SIZE].iter().enumerate() {
                    assert_eq!(*actual, pattern_stream_tail(index), "tail byte {index}");
                }
            }
        } else {
            let expected: Vec<u8> = (0..chunk.stored_size as usize)
                .map(|index| {
                    if *expected_offset == STREAM_HEAD_OFFSET {
                        pattern_stream_head(index)
                    } else {
                        (index as u8) ^ 0x5a
                    }
                })
                .collect();
            assert_eq!(stored, expected, "RAW chunk {id} byte readback");
        }
    }
    println!("C indexes match the pinned Kraken writer for three files and four mixed chunks; pinned AMPR readers validate and decode the codec-2 chunks");
}
