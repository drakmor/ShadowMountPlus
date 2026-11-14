use kraken_mount_pkg::decoder::{self, Desc, Scratch};
use std::io::{Read, Write};

fn get_le32(bytes: &[u8]) -> u32 {
    u32::from_le_bytes(bytes.try_into().unwrap())
}

fn main() {
    let mut input = Vec::new();
    std::io::stdin().read_to_end(&mut input).unwrap();
    let mut output_stream = std::io::stdout().lock();
    let mut scratch = Scratch::new_boxed();
    let mut at = 0usize;

    while at < input.len() {
        assert!(input.len() - at >= 13, "truncated differential input header");
        let stored_size = get_le32(&input[at..at + 4]) as usize;
        let output_size = get_le32(&input[at + 4..at + 8]) as usize;
        let even_length = get_le32(&input[at + 8..at + 12]);
        let mode = input[at + 12];
        at += 13;
        assert!(input.len() - at >= stored_size, "truncated stored bytes");
        let stored = &input[at..at + stored_size];
        at += stored_size;

        let mut output = vec![0xa5; output_size];
        let desc = Desc { even_len: even_length, mode };
        let accepted = decoder::decode_block(stored, desc, &mut output, &mut scratch).is_ok();
        output_stream.write_all(&[u8::from(accepted)]).unwrap();
        if accepted {
            output_stream.write_all(&output).unwrap();
        }
    }
}
