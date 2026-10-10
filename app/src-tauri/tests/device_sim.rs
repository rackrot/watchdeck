//! A fake device driving the daemon over a real socket, without mDNS.

use std::net::SocketAddr;
use std::sync::Arc;
use std::time::{Duration, Instant};

use futures_util::{SinkExt, StreamExt};
use tokio::net::TcpStream;
use tokio_tungstenite::tungstenite::Message as Ws;
use tokio_tungstenite::{connect_async, MaybeTlsStream, WebSocketStream};
use watchdeck_lib::daemon::actions::{ActionResult, Runner};
use watchdeck_lib::daemon::protocol::{ErrorCode, Message};
use watchdeck_lib::daemon::server::{DaemonEvent, Server};

type Client = WebSocketStream<MaybeTlsStream<TcpStream>>;

const DEVICE: &str = "wd-000000000001";

fn stub_runner() -> Runner {
    Arc::new(|btn, device| {
        Box::pin(async move {
            ActionResult {
                ok: true,
                code: 0,
                text: format!("ran {btn} for {device}"),
                ms: 1,
            }
        })
    })
}

async fn start() -> (SocketAddr, tokio::sync::broadcast::Receiver<DaemonEvent>) {
    let server = Server::bind("127.0.0.1:0", stub_runner()).await.unwrap();
    let addr = server.local_addr().unwrap();
    let events = server.subscribe();
    tokio::spawn(server.run());
    (addr, events)
}

async fn connect(addr: SocketAddr) -> Client {
    connect_async(format!("ws://{addr}/")).await.unwrap().0
}

fn hello(v: u32, device: &str) -> Ws {
    Ws::text(
        serde_json::to_string(&Message::Hello {
            v,
            device: device.into(),
            fw: "0.2.0".into(),
            screen: [1024, 600],
        })
        .unwrap(),
    )
}

fn press(btn: &str, seq: u64) -> Ws {
    Ws::text(
        serde_json::to_string(&Message::Press {
            btn: btn.into(),
            seq,
        })
        .unwrap(),
    )
}

async fn recv(c: &mut Client) -> Message {
    loop {
        match c.next().await.expect("stream ended").unwrap() {
            Ws::Text(t) => return serde_json::from_str(t.as_str()).unwrap(),
            Ws::Ping(_) | Ws::Pong(_) => {}
            other => panic!("unexpected frame {other:?}"),
        }
    }
}

/// Reads until the server ends the connection, however it chooses to (close frame, EOF or reset).
async fn expect_closed(c: &mut Client) {
    loop {
        match c.next().await {
            None | Some(Err(_)) | Some(Ok(Ws::Close(_))) => return,
            Some(Ok(_)) => {}
        }
    }
}

async fn handshake(addr: SocketAddr, device: &str) -> Client {
    let mut c = connect(addr).await;
    c.send(hello(1, device)).await.unwrap();
    assert!(matches!(recv(&mut c).await, Message::Welcome { .. }));
    assert!(matches!(recv(&mut c).await, Message::Layout { .. }));
    c
}

#[tokio::test]
async fn hello_gets_welcome_then_the_layout() {
    let (addr, _events) = start().await;
    let mut c = connect(addr).await;
    c.send(hello(1, DEVICE)).await.unwrap();

    let Message::Welcome { v, name } = recv(&mut c).await else {
        panic!("expected welcome")
    };
    assert_eq!(v, 1);
    assert!(!name.is_empty());

    let Message::Layout {
        cols,
        rows,
        buttons,
        ..
    } = recv(&mut c).await
    else {
        panic!("expected layout")
    };
    assert_eq!((cols, rows), (5, 3));
    assert_eq!(buttons.len(), 1);
    let b = &buttons[0];
    assert_eq!((b.id.as_str(), b.col, b.row), ("hello", 0, 0));
    assert_eq!((b.label.as_str(), b.bg, b.fg), ("Hello", 1959605, 16777215));
}

#[tokio::test]
async fn press_hello_gets_one_ok_result() {
    let (addr, _events) = start().await;
    let mut c = handshake(addr, DEVICE).await;
    c.send(press("hello", 17)).await.unwrap();
    assert_eq!(
        recv(&mut c).await,
        Message::Result {
            btn: "hello".into(),
            seq: 17,
            ok: true,
            code: 0,
            text: format!("ran hello for {DEVICE}"),
            ms: 1,
        }
    );
}

#[tokio::test]
async fn press_unknown_button_fails_without_running_anything() {
    let (addr, _events) = start().await;
    let mut c = handshake(addr, DEVICE).await;
    c.send(press("nope", 3)).await.unwrap();
    assert_eq!(
        recv(&mut c).await,
        Message::Result {
            btn: "nope".into(),
            seq: 3,
            ok: false,
            code: -1,
            text: "unknown button".into(),
            ms: 0,
        }
    );
}

#[tokio::test]
async fn unknown_message_type_does_not_end_the_session() {
    let (addr, _events) = start().await;
    let mut c = handshake(addr, DEVICE).await;
    c.send(Ws::text(r#"{"t":"future","x":1}"#)).await.unwrap();
    c.send(press("hello", 1)).await.unwrap();
    assert!(matches!(
        recv(&mut c).await,
        Message::Result { ok: true, .. }
    ));
}

#[tokio::test]
async fn wrong_version_gets_an_error_then_close() {
    let (addr, _events) = start().await;
    let mut c = connect(addr).await;
    c.send(hello(2, DEVICE)).await.unwrap();
    let Message::Error { code, .. } = recv(&mut c).await else {
        panic!("expected error")
    };
    assert_eq!(code, ErrorCode::Version);
    expect_closed(&mut c).await;
}

#[tokio::test]
async fn malformed_hello_gets_bad_request_then_close() {
    let (addr, _events) = start().await;
    let mut c = connect(addr).await;
    c.send(Ws::text("not json")).await.unwrap();
    let Message::Error { code, .. } = recv(&mut c).await else {
        panic!("expected error")
    };
    assert_eq!(code, ErrorCode::BadRequest);
    expect_closed(&mut c).await;
}

// The two timeout tests run on the real clock. A paused clock jumps ahead whenever the runtime
// waits on socket I/O, which fires the daemon's timers before the traffic under test arrives.
#[tokio::test]
async fn silence_after_connecting_closes_after_five_seconds() {
    let (addr, _events) = start().await;
    let mut c = connect(addr).await;
    let started = Instant::now();
    expect_closed(&mut c).await;
    let waited = started.elapsed();
    assert!(waited >= Duration::from_secs(5), "{waited:?}");
    assert!(waited < Duration::from_secs(6), "{waited:?}");
}

#[tokio::test]
async fn a_session_silent_for_thirty_seconds_is_dropped() {
    let (addr, mut events) = start().await;
    let mut c = handshake(addr, DEVICE).await;
    assert_eq!(
        events.recv().await.unwrap(),
        DaemonEvent::DeviceConnected(DEVICE.into())
    );
    let started = Instant::now();
    expect_closed(&mut c).await;
    let waited = started.elapsed();
    assert!(waited >= Duration::from_secs(29), "{waited:?}");
    assert!(waited < Duration::from_secs(31), "{waited:?}");
    assert_eq!(
        events.recv().await.unwrap(),
        DaemonEvent::DeviceDisconnected(DEVICE.into())
    );
}

#[tokio::test]
async fn pings_are_answered_with_pongs() {
    let (addr, _events) = start().await;
    let mut c = handshake(addr, DEVICE).await;
    c.send(Ws::Ping(vec![7].into())).await.unwrap();
    let frame = c.next().await.unwrap().unwrap();
    assert!(matches!(frame, Ws::Pong(p) if p.as_ref() == [7]));
}

#[tokio::test]
async fn a_message_over_16_kb_closes_the_session() {
    let (addr, _events) = start().await;
    let mut c = handshake(addr, DEVICE).await;
    let big = format!(
        r#"{{"t":"press","btn":"{}","seq":1}}"#,
        "a".repeat(16 * 1024)
    );
    c.send(Ws::text(big)).await.unwrap();
    expect_closed(&mut c).await;
}

#[tokio::test]
async fn reconnecting_device_replaces_its_old_session() {
    let (addr, mut events) = start().await;
    let mut old = handshake(addr, DEVICE).await;
    let mut new = handshake(addr, DEVICE).await;

    expect_closed(&mut old).await;

    // The new session still works.
    new.send(press("hello", 1)).await.unwrap();
    assert!(matches!(
        recv(&mut new).await,
        Message::Result { ok: true, .. }
    ));

    assert_eq!(
        events.recv().await.unwrap(),
        DaemonEvent::DeviceConnected(DEVICE.into())
    );
    assert_eq!(
        events.recv().await.unwrap(),
        DaemonEvent::DeviceDisconnected(DEVICE.into())
    );
    assert_eq!(
        events.recv().await.unwrap(),
        DaemonEvent::DeviceConnected(DEVICE.into())
    );
}
