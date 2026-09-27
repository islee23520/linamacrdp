//! Server-side MS-RDPEAI audio input over the `AUDIO_INPUT` dynamic channel.
//! The client captures audio; this processor negotiates 48 kHz mono 16-bit PCM
//! and delivers the client's PCM bytes to a per-connection sink.

use ironrdp_core::{impl_as_any, Encode, EncodeResult, WriteCursor};
use ironrdp_dvc::{DvcEncode, DvcMessage, DvcProcessor, DvcServerProcessor};
use ironrdp_pdu::PduResult;
use tracing::warn;

/// FreeRDP's `AUDIN_DVC_CHANNEL_NAME` (not its plugin name `audin`).
pub const AUDIO_INPUT_CHANNEL_NAME: &str = "AUDIO_INPUT";

const VERSION: u32 = 2;
const PCM_FORMAT: [u8; 18] = [
    1, 0, // WAVE_FORMAT_PCM
    1, 0, // mono
    0x80, 0xbb, 0, 0, // 48,000 Hz
    0, 0x77, 1, 0, // 96,000 bytes/s
    2, 0, // block alignment
    16, 0, // bits/sample
    0, 0, // cbSize
];
const MAX_FORMATS: usize = 64;
const MAX_PDU_SIZE: usize = 64 * 1024;
const FRAMES_PER_PACKET: u32 = 480;

const VERSION_ID: u8 = 1;
const FORMATS_ID: u8 = 2;
const OPEN_ID: u8 = 3;
const OPEN_REPLY_ID: u8 = 4;
const DATA_INCOMING_ID: u8 = 5;
const DATA_ID: u8 = 6;
const FORMAT_CHANGE_ID: u8 = 7;

/// Receives the selected PCM format and borrowed, unconverted interleaved
/// little-endian PCM data. The format callback precedes every sample callback.
pub trait AudinSampleSink: Send {
    fn on_format(&mut self, sample_rate: u32, channels: u16, bits_per_sample: u16);
    fn on_sample(&mut self, data: &[u8]);
}

/// Builds a fresh sink for each audio input DVC. Returning `None` discards
/// samples while still completing the protocol handshake.
pub trait AudinServerFactory: Send {
    fn build_sample_sink(&self) -> Option<Box<dyn AudinSampleSink>>;
}

struct AudinMessage {
    id: u8,
    body: Vec<u8>,
}

impl Encode for AudinMessage {
    fn encode(&self, dst: &mut WriteCursor<'_>) -> EncodeResult<()> {
        ironrdp_core::ensure_size!(in: dst, size: self.size());
        dst.write_u8(self.id);
        dst.write_slice(&self.body);
        Ok(())
    }

    fn name(&self) -> &'static str {
        "SNDIN_PDU"
    }

    fn size(&self) -> usize {
        1 + self.body.len()
    }
}

impl DvcEncode for AudinMessage {}

fn message(id: u8, body: Vec<u8>) -> DvcMessage {
    Box::new(AudinMessage { id, body })
}

#[derive(Clone, Copy, PartialEq, Eq)]
enum State {
    Version,
    Formats,
    Opening,
    Streaming,
    Stopped,
}

/// Per-connection server-direction audio input receiver.
pub struct AudinServer {
    sink: Option<Box<dyn AudinSampleSink>>,
    state: State,
    client_format_index: Option<u32>,
}

impl AudinServer {
    pub fn new(sink: Option<Box<dyn AudinSampleSink>>) -> Self {
        Self {
            sink,
            state: State::Version,
            client_format_index: None,
        }
    }

    fn reject(&self, reason: &str) -> PduResult<Vec<DvcMessage>> {
        warn!(reason, "MS-RDPEAI ignored invalid client PDU");
        Ok(Vec::new())
    }

    fn formats(&mut self, body: &[u8]) -> Option<u32> {
        let header = body.get(..8)?;
        let count = usize::try_from(u32::from_le_bytes(header[..4].try_into().ok()?)).ok()?;
        // cbSizeFormatsPacket is advisory in practice: FreeRDP itself repairs
        // mismatches. The parsed entry lengths are the actual trust boundary.
        let _size = u32::from_le_bytes(header[4..8].try_into().ok()?);
        if count == 0 || count > MAX_FORMATS {
            return None;
        }
        let mut pos = 8usize;
        let mut chosen = None;
        for index in 0..count {
            let fixed = body.get(pos..pos.checked_add(18)?)?;
            let extra = usize::from(u16::from_le_bytes(fixed[16..18].try_into().ok()?));
            let end = pos.checked_add(18)?.checked_add(extra)?;
            let entire = body.get(pos..end)?;
            if entire == PCM_FORMAT && chosen.is_none() {
                chosen = u32::try_from(index).ok();
            }
            pos = end;
        }
        chosen
    }
}

impl_as_any!(AudinServer);

impl DvcProcessor for AudinServer {
    fn channel_name(&self) -> &str {
        AUDIO_INPUT_CHANNEL_NAME
    }

    fn start(&mut self, _channel_id: u32) -> PduResult<Vec<DvcMessage>> {
        self.state = State::Version;
        self.client_format_index = None;
        Ok(vec![message(VERSION_ID, VERSION.to_le_bytes().to_vec())])
    }

    fn close(&mut self, _channel_id: u32) {
        self.state = State::Stopped;
    }

    fn process(&mut self, _channel_id: u32, payload: &[u8]) -> PduResult<Vec<DvcMessage>> {
        if payload.is_empty() || payload.len() > MAX_PDU_SIZE {
            return self.reject("empty or oversized PDU");
        }
        let (id, body) = (payload[0], &payload[1..]);
        match (self.state, id) {
            // FreeRDP announces an incoming Formats packet before sending
            // the packet itself (audin_process_formats in audin_main.c).
            (State::Formats, DATA_INCOMING_ID) if body.is_empty() => Ok(Vec::new()),
            (State::Version, VERSION_ID) if body.len() == 4 => {
                let version = u32::from_le_bytes([body[0], body[1], body[2], body[3]]);
                if version == 0 || version > VERSION {
                    return self.reject("unsupported version");
                }
                self.state = State::Formats;
                let mut formats = Vec::with_capacity(8 + PCM_FORMAT.len());
                formats.extend_from_slice(&1u32.to_le_bytes());
                // Reserved in server->client Sound Formats PDUs.
                formats.extend_from_slice(&0u32.to_le_bytes());
                formats.extend_from_slice(&PCM_FORMAT);
                Ok(vec![message(FORMATS_ID, formats)])
            }
            (State::Formats, FORMATS_ID) => {
                let Some(index) = self.formats(body) else {
                    return self.reject("malformed formats or no matching PCM format");
                };
                self.client_format_index = Some(index);
                self.state = State::Opening;
                let mut open = Vec::with_capacity(8 + PCM_FORMAT.len());
                open.extend_from_slice(&FRAMES_PER_PACKET.to_le_bytes());
                open.extend_from_slice(&index.to_le_bytes());
                open.extend_from_slice(&PCM_FORMAT);
                Ok(vec![message(OPEN_ID, open)])
            }
            (State::Opening | State::Streaming, FORMAT_CHANGE_ID) if body.len() == 4 => {
                let index = u32::from_le_bytes([body[0], body[1], body[2], body[3]]);
                if Some(index) != self.client_format_index {
                    return self.reject("unrequested format change");
                }
                Ok(Vec::new())
            }
            (State::Opening, OPEN_REPLY_ID) if body.len() == 4 => {
                let result = u32::from_le_bytes([body[0], body[1], body[2], body[3]]);
                if result != 0 {
                    self.state = State::Stopped;
                    return self.reject("client rejected audio input open");
                }
                // The initialFormat in Open already fixes the data format.
                // FreeRDP confirms it before OpenReply, but another client may
                // omit the redundant initial FormatChange entirely.
                self.state = State::Streaming;
                if let Some(sink) = self.sink.as_mut() {
                    sink.on_format(48_000, 1, 16);
                }
                Ok(Vec::new())
            }
            (State::Opening | State::Streaming, DATA_INCOMING_ID) if body.is_empty() => Ok(Vec::new()),
            (State::Streaming, DATA_ID) if !body.is_empty() && body.len() % 2 == 0 => {
                if let Some(sink) = self.sink.as_mut() {
                    sink.on_sample(body);
                }
                Ok(Vec::new())
            }
            _ => self.reject("unexpected message, state, or length"),
        }
    }
}

impl DvcServerProcessor for AudinServer {}

#[cfg(test)]
mod tests {
    use std::sync::{Arc, Mutex};

    use ironrdp_core::encode_vec;

    use super::*;

    #[derive(Default)]
    struct Captured {
        formats: Vec<(u32, u16, u16)>,
        samples: Vec<Vec<u8>>,
    }

    struct Sink(Arc<Mutex<Captured>>);

    impl AudinSampleSink for Sink {
        fn on_format(&mut self, sample_rate: u32, channels: u16, bits_per_sample: u16) {
            self.0
                .lock()
                .unwrap()
                .formats
                .push((sample_rate, channels, bits_per_sample));
        }

        fn on_sample(&mut self, data: &[u8]) {
            self.0.lock().unwrap().samples.push(data.to_vec());
        }
    }

    fn send(receiver: &mut AudinServer, id: u8, body: &[u8]) -> Vec<Vec<u8>> {
        let mut pdu = vec![id];
        pdu.extend_from_slice(body);
        receiver
            .process(1, &pdu)
            .unwrap()
            .into_iter()
            .map(|message| encode_vec(&*message).unwrap())
            .collect()
    }

    fn begin(receiver: &mut AudinServer, formats: &[u8], count: u32, size: u32) -> Vec<Vec<u8>> {
        assert_eq!(receiver.channel_name(), "AUDIO_INPUT");
        let version = receiver.start(1).unwrap();
        assert_eq!(encode_vec(&*version[0]).unwrap(), [VERSION_ID, 2, 0, 0, 0]);
        let advertised = send(receiver, VERSION_ID, &2u32.to_le_bytes());
        assert_eq!(advertised[0][0], FORMATS_ID);
        assert_eq!(&advertised[0][1..9], &[1, 0, 0, 0, 0, 0, 0, 0]);
        assert_eq!(&advertised[0][9..], &PCM_FORMAT);
        assert!(send(receiver, DATA_INCOMING_ID, &[]).is_empty());
        let mut list = Vec::new();
        list.extend_from_slice(&count.to_le_bytes());
        list.extend_from_slice(&size.to_le_bytes());
        list.extend_from_slice(formats);
        send(receiver, FORMATS_ID, &list)
    }

    #[test]
    fn receives_pcm_when_freerdp_confirms_format_before_open_reply() {
        let captured = Arc::new(Mutex::new(Captured::default()));
        let mut receiver = AudinServer::new(Some(Box::new(Sink(captured.clone()))));
        // First entry is a different valid PCM format; the second is ours.
        let mut formats = PCM_FORMAT.to_vec();
        formats[4] = 0x44;
        formats[5] = 0xac;
        formats.extend_from_slice(&PCM_FORMAT);
        let open = begin(&mut receiver, &formats, 2, 9 + 36);
        assert_eq!(open.len(), 1);
        assert_eq!(open[0][0], OPEN_ID);
        assert_eq!(&open[0][1..5], &FRAMES_PER_PACKET.to_le_bytes());
        assert_eq!(&open[0][5..9], &1u32.to_le_bytes());
        assert_eq!(&open[0][9..], &PCM_FORMAT);

        // FreeRDP audin_main.c sends FormatChange then OpenReply, followed by
        // DataIncoming and Data for each captured buffer.
        send(&mut receiver, FORMAT_CHANGE_ID, &1u32.to_le_bytes());
        send(&mut receiver, OPEN_REPLY_ID, &0u32.to_le_bytes());
        send(&mut receiver, DATA_INCOMING_ID, &[]);
        send(&mut receiver, DATA_ID, &[1, 2, 3, 4]);
        let data = captured.lock().unwrap();
        assert_eq!(data.formats, [(48_000, 1, 16)]);
        assert_eq!(data.samples, [vec![1, 2, 3, 4]]);
    }

    #[test]
    fn receives_pcm_without_initial_format_change_and_ignores_bad_size_hint() {
        let captured = Arc::new(Mutex::new(Captured::default()));
        let mut receiver = AudinServer::new(Some(Box::new(Sink(captured.clone()))));
        begin(&mut receiver, &PCM_FORMAT, 1, 0);
        send(&mut receiver, OPEN_REPLY_ID, &0u32.to_le_bytes());
        send(&mut receiver, DATA_ID, &[5, 6]);
        assert_eq!(captured.lock().unwrap().samples, [vec![5, 6]]);
    }

    #[test]
    fn rejects_truncated_formats_and_invalid_transitions_without_samples() {
        let captured = Arc::new(Mutex::new(Captured::default()));
        let mut receiver = AudinServer::new(Some(Box::new(Sink(captured.clone()))));
        receiver.start(1).unwrap();
        assert!(send(&mut receiver, DATA_ID, &[0, 0]).is_empty());
        send(&mut receiver, VERSION_ID, &2u32.to_le_bytes());
        let mut truncated = Vec::new();
        truncated.extend_from_slice(&2u32.to_le_bytes());
        truncated.extend_from_slice(&0u32.to_le_bytes());
        truncated.extend_from_slice(&PCM_FORMAT);
        assert!(send(&mut receiver, FORMATS_ID, &truncated).is_empty());
        assert!(send(&mut receiver, FORMATS_ID, &[0xff; 8]).is_empty());
        let open = send(
            &mut receiver,
            FORMATS_ID,
            &[1, 0, 0, 0, 0, 0, 0, 0]
                .iter()
                .copied()
                .chain(PCM_FORMAT)
                .collect::<Vec<_>>()
                .as_slice(),
        );
        assert_eq!(open.len(), 1);
        assert!(send(&mut receiver, FORMAT_CHANGE_ID, &99u32.to_le_bytes()).is_empty());
        assert!(send(&mut receiver, DATA_ID, &[0, 0]).is_empty());
        send(&mut receiver, OPEN_REPLY_ID, &1u32.to_le_bytes());
        assert!(send(&mut receiver, DATA_ID, &[0, 0]).is_empty());
        assert!(captured.lock().unwrap().samples.is_empty());
    }

    #[test]
    fn rejects_oversized_and_misaligned_data_without_delivering_it() {
        let captured = Arc::new(Mutex::new(Captured::default()));
        let mut receiver = AudinServer::new(Some(Box::new(Sink(captured.clone()))));
        begin(&mut receiver, &PCM_FORMAT, 1, 27);
        send(&mut receiver, OPEN_REPLY_ID, &0u32.to_le_bytes());
        send(&mut receiver, DATA_ID, &[1]);
        send(&mut receiver, DATA_ID, &vec![0; MAX_PDU_SIZE]);
        assert!(captured.lock().unwrap().samples.is_empty());
    }
}
