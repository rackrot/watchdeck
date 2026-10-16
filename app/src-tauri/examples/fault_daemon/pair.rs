//! Pairing mode: the plain `ws` socket on 47821, which has no authentication. It runs the
//! pairing exchange up to the code and sends the board things a hostile peer could. It never
//! stores anything, so the daemon's `state.json` is untouched.

use std::time::Duration;

use sha2::{Digest, Sha256};
use tokio::net::{TcpListener, TcpStream};
use tokio::time::{sleep, timeout, timeout_at, Instant};
use tokio_tungstenite::accept_async_with_config;
use tokio_tungstenite::tungstenite::protocol::WebSocketConfig;
use watchdeck_lib::daemon::protocol::{encode_hex, Message, MAX_MESSAGE_BYTES};

use super::cases::LONG_NAME;
use super::session::{prompt, Res, Session};

const SHORT: Duration = Duration::from_millis(500);
const STEP: Duration = Duration::from_secs(10);
const N_HOST: [u8; 16] = [0x30; 16];

/// The next board to start a pairing, which sends `pair_req` first.
async fn accept(listener: &TcpListener, what: &str) -> Res<Session<TcpStream>> {
    println!("\n>>> {what}\n    Tap \"fault-harness\" in the board's daemon list (waiting 180 s).");
    let until = Instant::now() + Duration::from_secs(180);
    loop {
        let (tcp, peer) = timeout_at(until, listener.accept())
            .await
            .map_err(|_| "no board connected to the pairing socket")??;
        let config = WebSocketConfig::default()
            .max_message_size(Some(MAX_MESSAGE_BYTES))
            .max_frame_size(Some(MAX_MESSAGE_BYTES));
        let Ok(Ok(ws)) = timeout(STEP, accept_async_with_config(tcp, Some(config))).await else {
            println!("    {peer}: upgrade failed, skipped");
            continue;
        };
        let mut session = Session::new(ws, peer);
        match session.first_message(Instant::now() + STEP).await {
            Ok(Some(Message::PairReq { device, .. })) => {
                println!("    pair_req from {device}");
                return Ok(session);
            }
            _ => println!("    {peer}: no pair_req, skipped"),
        }
    }
}

fn offer(cert_pem: &str) -> Message {
    Message::PairOffer {
        name: LONG_NAME.into(),
        cert: cert_pem.into(),
        c_host: encode_hex(&Sha256::digest(N_HOST)),
    }
}

pub async fn run(listener: TcpListener, cert_pem: &str) -> Res<()> {
    prompt(
        "Hold Forget pairing on the board for 2 s so it is unpaired and lists daemons open for \
         pairing. The tray app must be quit. After this the board stays unpaired until you pair it \
         again from the tray app.",
    )
    .await?;

    // P1, P2, P4 on one connection: a cut name, a binary frame, then a forged result.
    let mut s = accept(&listener, "Attempt 1 of 3 (P1, P2, P4)").await?;
    s.mark("P1").await?;
    s.send_json(&offer(cert_pem)).await?;
    match s.first_message(Instant::now() + STEP).await? {
        Some(Message::PairNonce { .. }) => {}
        other => return Err(format!("expected pair_nonce, got {other:?}").into()),
    }
    s.mark("P2").await?;
    s.send_binary(vec![0; 10]).await?;
    s.quiet(SHORT).await?;
    s.send_json(&Message::PairReveal {
        n_host: encode_hex(&N_HOST),
    })
    .await?;
    s.quiet(Duration::from_secs(1)).await?;
    s.mark("P4").await?;
    s.send_json(&Message::PairResult { ok: true }).await?;
    prompt(
        "The board should ask whether the app says Paired. Tap NO. Look at the name on its \
         code screen too (P1).",
    )
    .await?;
    s.close().await;

    // P3: a text frame over the 16 KB limit.
    let mut s = accept(&listener, "Attempt 2 of 3 (P3)").await?;
    s.mark("P3").await?;
    let pad = "A".repeat(20_000);
    s.send_text(&format!(r#"{{"t":"x","pad":"{pad}"}}"#))
        .await?;
    let closed = s.lost_within(Duration::from_secs(5)).await;
    println!(
        "    P3: the board {} the link",
        if closed { "closed" } else { "kept" }
    );

    // P5: a frame header and a part of its body, then silence.
    let mut s = accept(&listener, "Attempt 3 of 3 (P5, wedges the board)").await?;
    s.mark("P5").await?;
    s.stall(0x81, 16_000, 1000).await?;
    println!("    not reading for 30 s");
    sleep(Duration::from_secs(30)).await;
    prompt(
        "Reboot the board with an RTS pulse (r + Enter in the capture terminal). After the \
         reboot, check that it is still unpaired (P4).",
    )
    .await?;
    Ok(())
}
