use kraken_mount_pkg::decoder::{self, Desc, Scratch};

fn raw_header(out: &mut Vec<u8>, length: usize) {
    out.extend_from_slice(&[
        (length >> 16) as u8,
        (length >> 8) as u8,
        length as u8,
    ]);
}

fn decode(stored: &[u8], logical_size: usize, mode: u8) -> Vec<u8> {
    let mut output = vec![0u8; logical_size];
    let mut scratch = Scratch::new_boxed();
    let desc = Desc {
        even_len: stored.len() as u32,
        mode,
    };
    decoder::decode_block(stored, desc, &mut output, &mut scratch).unwrap();
    output
}

fn lz_literal_vector(delta: bool) -> (Vec<u8>, Vec<u8>) {
    let logical_size = 0x20000usize;
    let expected: Vec<u8> = (0..logical_size)
        .map(|i| ((i * 31 + (i / 29) * 7) & 255) as u8)
        .collect();
    let mut stored = expected[..8].to_vec();
    stored.push(0x80); // excess framing without length escapes
    raw_header(&mut stored, logical_size - 8);
    for i in 8..logical_size {
        stored.push(if delta {
            expected[i].wrapping_sub(expected[i - 8])
        } else {
            expected[i]
        });
    }
    raw_header(&mut stored, 0); // no commands
    raw_header(&mut stored, 0); // no offsets
    raw_header(&mut stored, 0); // no lengths
    (stored, expected)
}

fn escaped_match_vector() -> (Vec<u8>, Vec<u8>) {
    let mut expected = vec![0u8; 8 + 272];
    for i in 0..8 {
        expected[i] = 0x60 + i as u8;
    }
    for i in 8..expected.len() {
        expected[i] = expected[i - 8];
    }

    let mut stored = expected[..8].to_vec();
    stored.push(0x81); // one trailing byte contains the escaped length
    raw_header(&mut stored, 0); // no literals
    raw_header(&mut stored, 1);
    stored.push(0x3c); // no literals, recent distance 8, escaped match
    raw_header(&mut stored, 0); // no new offsets
    raw_header(&mut stored, 1);
    stored.push(0xff); // 255-byte base plus zero excess
    stored.push(0x80); // MSB length code 64, yielding zero excess
    (stored, expected)
}

fn main() {
    for delta in [false, true] {
        let (stored, expected) = lz_literal_vector(delta);
        let mode = 2 | if delta { 1 } else { 0 };
        assert_eq!(decode(&stored, expected.len(), mode), expected);
    }

    // One new traditional offset (distance 8), followed by 18 trailing literals.
    let mut expected = [0u8; 32];
    for i in 0..8 {
        expected[i] = 0x30 + i as u8;
    }
    expected.copy_within(0..6, 8);
    for i in 14..32 {
        expected[i] = (i * 9 + 1) as u8;
    }
    let mut stored = expected[..8].to_vec();
    stored.push(0x80);
    raw_header(&mut stored, 18);
    stored.extend_from_slice(&expected[14..]);
    raw_header(&mut stored, 1);
    stored.push(0xd0); // new offset, six-byte match, no leading literals
    raw_header(&mut stored, 1);
    stored.push(0); // traditional distance-eight code
    raw_header(&mut stored, 0);
    stored.push(0); // the forward stream's four offset bits
    assert_eq!(decode(&stored, expected.len(), 2), expected);

    let (stored, expected) = escaped_match_vector();
    assert_eq!(decode(&stored, expected.len(), 2), expected);

    // Type-2 Huffman array, two one-bit symbols over three interleaved streams.
    let mut stored = vec![0u8; 17];
    stored[0] = 0x20;
    stored[1..5].copy_from_slice(&((31u32 << 18) | 12).to_be_bytes());
    stored[5..].copy_from_slice(&[
        0x00, 0x82, 0x0a, 0x10, // table
        0x02, 0x00, // stream A byte count
        0x00, 0x00, // stream A
        0x00, 0x00, // stream C
        0x07, 0xff, // stream B, stored reversed
    ]);
    let expected: Vec<u8> = (0..32)
        .map(|i| if i % 3 == 1 { b'B' } else { b'A' })
        .collect();
    assert_eq!(decode(&stored, expected.len(), 0), expected);

    println!("pinned Kraken decoder accepted the checked synthetic block vectors");
}
