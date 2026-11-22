//! Reuse our named loopbacks. Ask the system for authorization only when their
//! count/permissions need changing. No GUI or capture process runs as root.
use std::{collections::BTreeMap, fs, os::unix::fs::MetadataExt, path::{Path,PathBuf}, process::Command};
fn scan()->Result<BTreeMap<u32,(PathBuf,bool)>,String> {
    let mut found=BTreeMap::new();
    let entries=match fs::read_dir("/sys/class/video4linux") {Ok(e)=>e,Err(e) if e.kind()==std::io::ErrorKind::NotFound=>return Ok(found),Err(e)=>return Err(e.to_string())};
    for entry in entries {
        let entry=entry.map_err(|e|e.to_string())?;
        let filename=entry.file_name();let Some(filename)=filename.to_str() else{continue;};
        if !filename.starts_with("video") || !filename[5..].chars().all(|c|c.is_ascii_digit()) {continue;}
        let label=fs::read_to_string(entry.path().join("name")).unwrap_or_default();
        let Some(index)=label.trim().strip_prefix("Capture View ").and_then(|v|v.parse::<u32>().ok()) else{continue;};
        if !(1..=8).contains(&index) {continue;}
        let target=fs::canonicalize(entry.path()).map_err(|e|e.to_string())?;
        if !target.starts_with("/sys/devices/virtual/video4linux") {continue;}
        let path=PathBuf::from("/dev").join(filename);
        let metadata=fs::metadata(&path).map_err(|e|e.to_string())?;
        let writable=metadata.uid()==unsafe{libc::geteuid()} && metadata.mode()&0o600==0o600;
        if found.insert(index,(path,writable)).is_some(){return Err("Duplicate Capture View virtual-camera names; close other instances first".into());}
    }
    Ok(found)
}
pub fn ensure_count(engine:&Path,count:u32)->Result<Vec<String>,String> {
    if count>8{return Err("Choose between 0 and 8 virtual video devices".into());}
    let found=scan()?;
    if found.len()==count as usize && (1..=count).all(|i|found.get(&i).is_some_and(|(_,w)|*w)) {
        return Ok(found.values().map(|(p,_)|p.to_string_lossy().into_owned()).collect());
    }
    let mut command=Command::new("pkexec");
    if let Some(appimage)=std::env::var_os("APPIMAGE").filter(|p|Path::new(p).is_file()) {
        // Re-enter the archive under the requesting user's authorization, not
        // the existing user-only FUSE mount. This also works without FUSE.
        command.arg(appimage).arg("--appimage-extract-and-run").arg("--setup-virtual-devices");
    } else {
        command.arg(engine.with_file_name("capture-view-device-helper")).arg("--count");
    }
    let result=command.arg(count.to_string()).output().map_err(|e|format!("Could not request virtual-camera setup: {e}. Install polkit and a matching v4l2loopback module."))?;
    if !result.status.success(){
        let message=String::from_utf8_lossy(&result.stderr).trim().to_owned();
        return Err(if message.is_empty(){"Virtual-camera authorization was cancelled or setup failed".into()}else{message});
    }
    let paths:Vec<String>=serde_json::from_slice(&result.stdout).map_err(|e|format!("Could not read virtual-camera setup result: {e}"))?;
    let verified=scan()?;
    if paths.len()!=count as usize || !(1..=count).all(|i|verified.get(&i).is_some_and(|(_,w)|*w)) {
        return Err("Virtual-camera setup did not provide all requested devices".into());
    }
    Ok((1..=count).map(|i|verified[&i].0.to_string_lossy().into_owned()).collect())
}
