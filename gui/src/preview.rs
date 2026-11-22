//! Newest-frame preview transport. No encoded stream and no unbounded frame queue.
//! CVP1: 64-byte header; three slots, each a 64-byte seqlock followed by BGRA.
//! Read through pread into owned buffers, never aliasing the producer's mapping.
use std::{fs::File, io, os::unix::fs::FileExt, path::PathBuf, sync::{Arc, Mutex, atomic::{AtomicBool, Ordering}}, thread::{self, JoinHandle}, time::Duration};

pub struct Frame { pub width:u32, pub height:u32, pub pixels:Vec<u8> }
struct Reader { file:File, sequence:u64 }
fn u32_at(bytes:&[u8],offset:usize)->u32 {u32::from_le_bytes(bytes[offset..offset+4].try_into().unwrap())}
fn u64_at(bytes:&[u8],offset:usize)->u64 {u64::from_le_bytes(bytes[offset..offset+8].try_into().unwrap())}
impl Reader {
    fn newest(&mut self)->io::Result<Option<Frame>> {
        let mut header=[0u8;64];self.file.read_exact_at(&mut header,0)?;
        if u32_at(&header,0)!=0x31505643 || u32_at(&header,4)!=1 {return Ok(None);}
        let (width,height,stride,slots)=(u32_at(&header,8),u32_at(&header,12),u32_at(&header,16),u32_at(&header,20));
        let capacity=u64_at(&header,24);let sequence=u64_at(&header,32);
        if sequence==0 || sequence==self.sequence {return Ok(None);}
        if sequence>u64::MAX/2 {return Err(io::Error::new(io::ErrorKind::InvalidData,"invalid preview sequence"));}
        let length=u64::from(stride)*u64::from(height);
        if width==0 || height==0 || width>16384 || height>16384 || stride!=width*4 || slots!=3 ||
            capacity>128*1024*1024 || length>capacity || capacity%64!=0 {
            return Err(io::Error::new(io::ErrorKind::InvalidData,"invalid preview dimensions"));
        }
        let offset=64+(sequence%3)*(64+capacity);
        if self.file.metadata()?.len()<64+3*(64+capacity) {return Ok(None);}
        let mut before=[0u8;8];self.file.read_exact_at(&mut before,offset)?;
        if u64_at(&before,0)!=sequence*2 {return Ok(None);}
        let mut pixels=vec![0;length as usize];self.file.read_exact_at(&mut pixels,offset+64)?;
        let mut after=[0u8;8];self.file.read_exact_at(&mut after,offset)?;
        if before!=after {return Ok(None);}
        self.sequence=sequence;
        Ok(Some(Frame{width,height,pixels}))
    }
}

pub struct Worker {
    pub visible:Arc<AtomicBool>,
    latest:Arc<Mutex<Option<Frame>>>,
    error:Arc<Mutex<Option<String>>>,
    stop:Arc<AtomicBool>,
    join:Option<JoinHandle<()>>,
    pub path:PathBuf,
}
impl Worker {
    pub fn new(path:PathBuf)->Self {
        let visible=Arc::new(AtomicBool::new(true));let latest=Arc::new(Mutex::new(None));
        let error=Arc::new(Mutex::new(None));let stop=Arc::new(AtomicBool::new(false));
        let (v,l,e,s,p)=(visible.clone(),latest.clone(),error.clone(),stop.clone(),path.clone());
        let join=thread::spawn(move || {
            let mut reader=None;
            while !s.load(Ordering::Acquire) {
                if !v.load(Ordering::Relaxed) {thread::sleep(Duration::from_millis(12));continue;}
                if reader.is_none() {
                    match File::open(&p) {
                        Ok(file)=>reader=Some(Reader{file,sequence:0}),
                        Err(err) if err.kind()==io::ErrorKind::NotFound=>{},
                        Err(err)=>{*e.lock().unwrap()=Some(format!("Could not open viewer preview: {err}"));break;}
                    }
                }
                if let Some(r)=&mut reader {
                    match r.newest() {
                        Ok(Some(frame))=>*l.lock().unwrap()=Some(frame),
                        Ok(None)=>{},
                        Err(err) if err.kind()==io::ErrorKind::UnexpectedEof=>{},
                        Err(err)=>{*e.lock().unwrap()=Some(format!("Viewer preview failed: {err}"));break;}
                    }
                }
                thread::sleep(Duration::from_millis(2));
            }
        });
        Self{visible,latest,error,stop,join:Some(join),path}
    }
    pub fn take(&self)->Option<Frame> {self.latest.lock().unwrap().take()}
    pub fn error(&self)->Option<String> {self.error.lock().unwrap().take()}
}
impl Drop for Worker {
    fn drop(&mut self){self.stop.store(true,Ordering::Release);if let Some(join)=self.join.take(){let _=join.join();}let _=std::fs::remove_file(&self.path);}
}


#[cfg(test)]
mod tests {
    use super::*;
    use std::{fs::OpenOptions, sync::atomic::AtomicU64};
    static NEXT:AtomicU64=AtomicU64::new(0);
    fn fixture()->(Reader,PathBuf) {
        let path=std::env::temp_dir().join(format!("capture-view-preview-test-{}-{}",std::process::id(),NEXT.fetch_add(1,Ordering::Relaxed)));
        let file=OpenOptions::new().create_new(true).read(true).write(true).open(&path).unwrap();
        file.set_len(448).unwrap();
        let mut header=[0u8;64];
        for (offset,value) in [(0,0x31505643u32),(4,1),(8,2),(12,2),(16,8),(20,3)] {
            header[offset..offset+4].copy_from_slice(&value.to_le_bytes());
        }
        header[24..32].copy_from_slice(&64u64.to_le_bytes());
        header[32..40].copy_from_slice(&1u64.to_le_bytes());
        file.write_all_at(&header,0).unwrap();
        file.write_all_at(&2u64.to_le_bytes(),192).unwrap();
        file.write_all_at(&[1,2,3,255,4,5,6,255,7,8,9,255,10,11,12,255],256).unwrap();
        (Reader{file,sequence:0},path)
    }
    #[test]
    fn reads_complete_bgra_frame_and_does_not_queue_duplicates() {
        let (mut reader,path)=fixture();
        let frame=reader.newest().unwrap().unwrap();
        assert_eq!((frame.width,frame.height),(2,2));
        assert_eq!(&frame.pixels[..4],&[1,2,3,255]);
        assert!(reader.newest().unwrap().is_none());
        std::fs::remove_file(path).unwrap();
    }
    #[test]
    fn rejects_busy_or_overwritten_slots() {
        let (mut reader,path)=fixture();
        reader.file.write_all_at(&3u64.to_le_bytes(),192).unwrap();
        assert!(reader.newest().unwrap().is_none());
        reader.file.write_all_at(&4u64.to_le_bytes(),192).unwrap();
        assert!(reader.newest().unwrap().is_none());
        std::fs::remove_file(path).unwrap();
    }
    #[test]
    fn rejects_invalid_layout_before_allocating_a_frame() {
        let (mut reader,path)=fixture();
        reader.file.write_all_at(&0u32.to_le_bytes(),8).unwrap();
        assert_eq!(reader.newest().err().unwrap().kind(),io::ErrorKind::InvalidData);
        std::fs::remove_file(path).unwrap();
    }
}
