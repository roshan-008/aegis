use aegis_rust::serve_store;
use std::path::Path;

fn main() -> Result<(), Box<dyn std::error::Error>> {
    let args: Vec<String> = std::env::args().collect();
    if args.len() < 3 || args.len() > 4 {
        eprintln!("usage: feed_server STORE_DIR PORT [ticks_per_sec]");
        std::process::exit(2);
    }
    let rate = args.get(3).map(|v| v.parse()).transpose()?.unwrap_or(0.0);
    let sent = serve_store(Path::new(&args[1]), args[2].parse()?, rate)?;
    println!("sent {sent} frames");
    Ok(())
}
