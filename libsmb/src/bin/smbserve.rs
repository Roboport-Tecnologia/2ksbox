//! Serves one folder over SMB, for trying clients against the library.
//!
//!   smbserve [--tcp ADDR | --unix PATH] [--share NAME] [--user U] [--password P]
//!            [--max-dialect 2.1|3.1.1|...] [--read-only] [--no-leases] [-v] DIR

use libsmb::{Account, Config, Dialect, Server, Share};
use std::sync::Arc;

fn main() {
    let mut args = std::env::args().skip(1);
    let (mut tcp, mut unix, mut dir) = (None, None, None);
    let (mut share, mut user, mut pass) = ("host".to_string(), "smb".to_string(), "smb".to_string());
    let (mut max, mut ro, mut verbose, mut leases) = (Dialect::Smb311, false, false, true);
    while let Some(a) = args.next() {
        let mut val = || args.next().unwrap_or_else(|| usage(&format!("{} wants a value", a)));
        match a.as_str() {
            "--tcp" => tcp = Some(val()),
            "--unix" => unix = Some(val()),
            "--share" => share = val(),
            "--user" => user = val(),
            "--password" => pass = val(),
            "--max-dialect" => max = val().parse().unwrap_or_else(|e: String| usage(&e)),
            "--read-only" => ro = true,
            "--no-leases" => leases = false,
            "-v" => verbose = true,
            _ if a.starts_with('-') => usage(&format!("unknown option {}", a)),
            _ => dir = Some(a),
        }
    }
    let dir = dir.unwrap_or_else(|| usage("no folder"));
    let t0 = std::time::Instant::now();
    let cfg = Config::new("smbserve", Account::new(&user, &pass))
        .share(Share::new(&share, &dir).read_only(ro))
        .max_dialect(max)
        .leases(leases)
        .logger(Arc::new(move |chatty, msg| {
            if verbose || !chatty {
                eprintln!("smbserve: {:9.3} {}", t0.elapsed().as_secs_f64(), msg);
            }
        }));
    let server = Server::new(cfg);
    let r = match (tcp, unix) {
        #[cfg(unix)]
        (None, Some(p)) => server.listen_unix(p),
        (t, None) => server.listen_tcp(t.as_deref().unwrap_or("127.0.0.1:4450")),
        _ => usage("--tcp or --unix, not both"),
    };
    if let Err(e) = r {
        eprintln!("smbserve: {}", e);
        std::process::exit(1);
    }
}

fn usage(why: &str) -> ! {
    eprintln!("smbserve: {}", why);
    eprintln!("usage: smbserve [--tcp ADDR | --unix PATH] [--share NAME] [--user U] [--password P] [--max-dialect D] [--read-only] [--no-leases] [-v] DIR");
    std::process::exit(2)
}
