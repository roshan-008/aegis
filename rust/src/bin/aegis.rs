use aegis_rust::{list_segments, recover_store, Segment};
use std::path::Path;

fn usage() -> ! {
    eprintln!("usage: aegis stats | inspect STORE_DIR | recover STORE_DIR");
    std::process::exit(2);
}

fn main() -> Result<(), Box<dyn std::error::Error>> {
    let args: Vec<String> = std::env::args().collect();
    match args.as_slice() {
        [_, command] if command == "stats" => {
            println!("aegis hybrid runtime: Rust storage/network + C++20 kernels");
            println!("registered_kernels 12");
            println!("safe_storage true\nsafe_feed_server true\nsegment_format AEGISSEG/v1");
        }
        [_, command, directory] if command == "inspect" => {
            let mut rows = 0usize;
            for path in list_segments(Path::new(directory))? {
                let segment = Segment::open(&path)?;
                let meta = segment.meta();
                rows += meta.n;
                println!(
                    "{} rows={} ts=[{},{}]",
                    path.display(),
                    meta.n,
                    meta.minimum[2],
                    meta.maximum[2]
                );
            }
            println!("total_rows {rows}");
        }
        [_, command, directory] if command == "recover" => {
            println!(
                "removed_torn_tails {}",
                recover_store(Path::new(directory))?
            );
        }
        _ => usage(),
    }
    Ok(())
}
