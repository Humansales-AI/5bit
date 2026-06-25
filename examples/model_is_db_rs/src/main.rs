// 5bit Model-IS-Database — Rust + ndarray
use ndarray::{Array1, Array2, Axis};
use ndarray_rand::RandomExt;
use rand::distributions::Uniform;
use rand::Rng;
use std::collections::HashMap;
use std::time::Instant;

const VOCAB: usize = 32;
const D_MODEL: usize = 64;
const MAX_SEQ: usize = 64;

struct ModelIsDB {
    embed: Array2<f32>,
    pos: Array2<f32>,
    w_q: Array2<f32>,
    w_k: Array2<f32>,
    w_v: Array2<f32>,
    w_o: Array2<f32>,
    w_out: Array2<f32>,
    b_out: Array1<f32>,
}

impl ModelIsDB {
    fn new() -> Self {
        let dist = Uniform::new(-0.02, 0.02);
        let mut pos = Array2::zeros((MAX_SEQ, D_MODEL));
        for p in 0..MAX_SEQ {
            for i in 0..D_MODEL {
                if i % 2 == 0 {
                    pos[[p, i]] = (p as f32 / (10000_f32.powf(i as f32 / D_MODEL as f32))).sin();
                } else {
                    pos[[p, i]] = (p as f32 / (10000_f32.powf((i - 1) as f32 / D_MODEL as f32))).cos();
                }
            }
        }
        Self {
            embed: Array2::random((VOCAB, D_MODEL), dist) * 0.1,
            pos,
            w_q: Array2::random((D_MODEL, D_MODEL), dist),
            w_k: Array2::random((D_MODEL, D_MODEL), dist),
            w_v: Array2::random((D_MODEL, D_MODEL), dist),
            w_o: Array2::random((D_MODEL, D_MODEL), dist),
            w_out: Array2::random((D_MODEL, VOCAB), dist),
            b_out: Array1::zeros(VOCAB),
        }
    }

    fn forward(&self, tokens: &[usize]) -> Array1<f32> {
        let n = tokens.len().min(MAX_SEQ);
        if n == 0 { return Array1::zeros(VOCAB); }
        let mut x = Array2::zeros((n, D_MODEL));
        for i in 0..n {
            let t = tokens[i].min(VOCAB - 1);
            for j in 0..D_MODEL { x[[i, j]] = self.embed[[t, j]] + self.pos[[i, j]]; }
        }
        let q = x.dot(&self.w_q); let k = x.dot(&self.w_k); let v = x.dot(&self.w_v);
        let scale = (D_MODEL as f32).sqrt();
        let scores = q.dot(&k.t()) / scale;
        let max_scores = scores.map_axis(Axis(1), |r| *r.iter().max_by(|a, b| a.partial_cmp(b).unwrap()).unwrap());
        let exp = (&scores - &max_scores.insert_axis(Axis(1))).mapv(|e| e.exp());
        let sum_exp = exp.sum_axis(Axis(1)).insert_axis(Axis(1));
        let attn = &exp / &sum_exp;
        let out = attn.dot(&v);
        let residual = &x + &(out.dot(&self.w_o) * 0.1);
        let sq = &residual * &residual;
        let norm = sq.sum_axis(Axis(1)).mapv(|v| (v + 1e-8).sqrt()).insert_axis(Axis(1));
        x = &residual / &norm;
        let last = x.row(n - 1);
        let logits = last.dot(&self.w_out) + &self.b_out;
        let max_val = logits.iter().cloned().fold(f32::NEG_INFINITY, f32::max);
        let exp = logits.mapv(|e| (e - max_val).exp());
        let sum = exp.sum();
        exp / sum
    }

    fn train_batch(&mut self, q_batch: &[Vec<usize>], a_batch: &[Vec<usize>], lr: f32) -> f32 {
        let mut total = 0.0;
        let scale = (D_MODEL as f32).sqrt();

        for (q, a) in q_batch.iter().zip(a_batch.iter()) {
            let mut ctx = q.clone();
            for &target in a {
                if target >= VOCAB { continue; }

                // ── Forward pass ──────────────────────────────────
                let n = ctx.len().min(MAX_SEQ);
                let mut x = Array2::zeros((n, D_MODEL));
                for i in 0..n {
                    let t = ctx[i].min(VOCAB - 1);
                    for j in 0..D_MODEL { x[[i, j]] = self.embed[[t, j]] + self.pos[[i, j]]; }
                }

                let q_val = x.dot(&self.w_q);  // [n, D]
                let k_val = x.dot(&self.w_k);
                let v_val = x.dot(&self.w_v);
                let scores = q_val.dot(&k_val.t()) / scale;
                let max_s = scores.map_axis(Axis(1), |r| *r.iter().max_by(|a,b| a.partial_cmp(b).unwrap()).unwrap());
                let exp_s = (&scores - &max_s.insert_axis(Axis(1))).mapv(|e| e.exp());
                let sum_s = exp_s.sum_axis(Axis(1)).insert_axis(Axis(1));
                let attn = &exp_s / &sum_s;  // [n, n]
                let out = attn.dot(&v_val);   // [n, D]
                let final_h = &x + &(out.dot(&self.w_o) * 0.1);  // residual

                let last = final_h.row(n - 1);
                let logits = last.dot(&self.w_out) + &self.b_out;
                let max_l = logits.iter().cloned().fold(f32::NEG_INFINITY, f32::max);
                let el = logits.mapv(|e| (e - max_l).exp());
                let sl = el.sum();
                let probs = &el / sl;

                let p = probs[target].max(1e-10);
                total -= p.ln();

                // ── Backward: output layer ──────────────────────
                let mut d_logits = probs.clone();
                d_logits[target] -= 1.0;
                for j in 0..D_MODEL {
                    for k in 0..VOCAB {
                        self.w_out[[j, k]] -= lr * d_logits[k] * last[j];
                    }
                }
                for k in 0..VOCAB { self.b_out[k] -= lr * d_logits[k]; }

                // Gradient to last hidden: dL/d_last = W_out @ d_logits
                let mut d_last = Array1::zeros(D_MODEL);
                for j in 0..D_MODEL {
                    for k in 0..VOCAB { d_last[j] += self.w_out[[j, k]] * d_logits[k]; }
                }

                // ── Backward: attention layer ───────────────────
                // dL/d_out = d_last (residual passes through)
                // dL/d_attn = d_out @ V^T
                let mut d_attn = Array2::zeros((n, n));
                for a_idx in 0..n {
                    for b_idx in 0..n {
                        for d in 0..D_MODEL {
                            d_attn[[a_idx, b_idx]] += d_last[d] * v_val[[b_idx, d]] * 0.1;
                        }
                    }
                }

                // Gradient through softmax: d_scores = attn * (d_attn - sum(d_attn * attn))
                let attn_d_attn: Array1<f32> = (0..n).map(|i| (0..n).map(|j| attn[[i, j]] * d_attn[[i, j]]).sum::<f32>()).collect();
                let mut d_scores = Array2::zeros((n, n));
                for i in 0..n {
                    for j in 0..n {
                        d_scores[[i, j]] = attn[[i, j]] * (d_attn[[i, j]] - attn_d_attn[i]);
                    }
                }
                let d_scores = &d_scores / scale;

                // Gradient through Q,K,V projections
                // d_Q = d_scores @ K, d_K = d_scores^T @ Q
                let d_q = d_scores.dot(&k_val);
                let d_k = d_scores.t().dot(&q_val);
                let d_v = attn.t().dot(&final_h);  // simplified — uses final hidden

                // Update weights: d_W = x^T @ d_proj
                for i in 0..D_MODEL {
                    for j in 0..D_MODEL {
                        let mut g_q = 0.0f32; let mut g_k = 0.0f32; let mut g_v = 0.0f32;
                        for a_idx in 0..n {
                            g_q += x[[a_idx, i]] * d_q[[a_idx, j]];
                            g_k += x[[a_idx, i]] * d_k[[a_idx, j]];
                            g_v += x[[a_idx, i]] * d_v[[a_idx, j]];
                        }
                        self.w_q[[i, j]] -= lr * 0.1 * g_q / n as f32;
                        self.w_k[[i, j]] -= lr * 0.1 * g_k / n as f32;
                        self.w_v[[i, j]] -= lr * 0.1 * g_v / n as f32;
                    }
                }

                // Embedding gradient
                for i in 0..n {
                    let t = ctx[i].min(VOCAB - 1);
                    for j in 0..D_MODEL {
                        self.embed[[t, j]] -= lr * 0.01 * d_last[j] / n as f32;
                    }
                }

                ctx.push(target);
            }
        }
        total
    }
}

fn encode_integer(val: i32) -> Vec<usize> {
    if val == 0 { return vec![0, 30]; }
    let s = val.abs().to_string();
    let mut t = Vec::new();
    for ch in s.chars() {
        let d = ch.to_digit(10).unwrap() as usize;
        t.push(if val >= 0 { d } else { 16 + d });
    }
    t.push(30); t
}

fn encode_word(s: &str) -> Vec<usize> {
    let mut t = vec![31];
    for ch in s.chars() {
        if ch.is_ascii_uppercase() { t.push((ch as usize - 'A' as usize) % 26); }
        else if ch.is_ascii_lowercase() { t.push((ch as usize - 'a' as usize) % 26); }
        else if ch == '-' { t.push(27); }
    }
    t.push(30); t
}

fn main() {
    println!("{}", "═".repeat(60));
    println!("  5bit Model-IS-DB — Rust + ndarray");
    println!("{}", "═".repeat(60));

    for (n_users, n_orders, epochs) in [(50, 200, 50), (100, 500, 30), (200, 1000, 20)] {
        println!("\n── {} users × {} orders ──", n_users, n_orders);
        let mut rng = rand::thread_rng();
        let mut db: HashMap<usize, Vec<i32>> = HashMap::new();
        for _ in 0..n_orders {
            let uid = rng.gen_range(1..=n_users);
            db.entry(uid).or_default().push(rng.gen_range(100..50000));
        }
        let mut qa = Vec::new();
        for uid in 1..=n_users {
            let c = db.get(&uid).map(|o| o.len()).unwrap_or(0);
            let mut q = encode_word("how-many-orders");
            q.extend(encode_integer(uid as i32));
            qa.push((q, encode_integer(c as i32)));
        }

        let mut model = ModelIsDB::new();
        let split = qa.len() / 2;
        let t0 = Instant::now();
        for _ in 0..epochs {
            let bq: Vec<Vec<usize>> = qa[..split].iter().map(|(q, _)| q.clone()).collect();
            let ba: Vec<Vec<usize>> = qa[..split].iter().map(|(_, a)| a.clone()).collect();
            model.train_batch(&bq, &ba, 0.01);
        }
        let train_t = t0.elapsed().as_secs_f64();
        let mut correct = 0; let mut total = 0;
        for (q, a) in &qa[split..] {
            if a.is_empty() { continue; }
            let probs = model.forward(q);
            let pred = probs.iter().enumerate().max_by(|(_, a), (_, b)| a.partial_cmp(b).unwrap()).unwrap().0;
            if pred == a[0] { correct += 1; }
            total += 1;
        }
        println!("  Train: {:.1}s  Acc: {:.1}%", train_t, 100.0 * correct as f64 / total as f64);
    }
    println!("\n═══ Rust + ndarray — 5-bit model ≈ database ═══");
}
