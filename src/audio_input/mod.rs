//! Client microphone PCM (MS-RDPEAI) into the macrdp Core Audio input driver.

use anyhow::{Context, Result};
use ironrdp_server::{AudinSampleSink, AudinServerFactory};
use tokio::io::AsyncWriteExt;
use tokio::sync::mpsc;
use tracing::{info, warn};

const MIC_ADDR: &str = "127.0.0.1:49228";
const QUEUE_PACKETS: usize = 16;

pub struct MacAudin {
    samples: mpsc::Sender<Vec<u8>>,
}

impl MacAudin {
    pub fn new() -> Result<Self> {
        let listener = std::net::TcpListener::bind(MIC_ADDR)
            .with_context(|| format!("binding macrdp Microphone feed at {MIC_ADDR}"))?;
        listener.set_nonblocking(true)?;
        let listener = tokio::net::TcpListener::from_std(listener)?;
        let (samples, mut receiver) = mpsc::channel::<Vec<u8>>(QUEUE_PACKETS);
        tokio::spawn(async move {
            loop {
                let (mut driver, _) = match listener.accept().await {
                    Ok(peer) => peer,
                    Err(error) => {
                        warn!(%error, "microphone driver connection failed");
                        continue;
                    }
                };
                info!("macrdp Microphone driver connected");
                // Samples buffered while the driver was disconnected are stale.
                while receiver.try_recv().is_ok() {}
                loop {
                    tokio::select! {
                        biased;
                        ready = driver.readable() => {
                            let mut probe = [0; 1];
                            if ready.is_err() || driver.try_read(&mut probe).ok() == Some(0) {
                                break;
                            }
                        }
                        pcm = receiver.recv() => {
                            let Some(pcm) = pcm else { return };
                            if let Err(error) = driver.write_all(&pcm).await {
                                warn!(%error, "microphone driver disconnected");
                                break;
                            }
                        }
                    }
                }
            }
        });
        Ok(Self { samples })
    }
}

struct MicSink {
    samples: mpsc::Sender<Vec<u8>>,
}

impl AudinSampleSink for MicSink {
    fn on_format(&mut self, sample_rate: u32, channels: u16, bits_per_sample: u16) {
        info!(
            sample_rate,
            channels, bits_per_sample, "client microphone format opened"
        );
    }

    fn on_sample(&mut self, data: &[u8]) {
        // Never stall the RDP receive loop when the driver is absent or slow.
        let _ = self.samples.try_send(data.to_vec());
    }
}

impl AudinServerFactory for MacAudin {
    fn build_sample_sink(&self) -> Option<Box<dyn AudinSampleSink>> {
        Some(Box::new(MicSink {
            samples: self.samples.clone(),
        }))
    }
}
