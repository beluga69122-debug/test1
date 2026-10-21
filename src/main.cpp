#include <Geode/Geode.hpp>
#include <Geode/modify/PlayerObject.hpp>
#include <vector>
#include <deque>
#include <cmath>
#include <algorithm>

using namespace geode::prelude;

static constexpr int SAVED_LABEL_TAG = 0x5AFE;

// Times are in "frames" (1 frame = 1/60 s). One physics tick at 240 TPS = 0.25 frames.
static constexpr float TICK = 0.25f;
static constexpr int MAX_EVENTS_PER_TICK = 6;
static constexpr int SETTLE_TICKS = 6;
static constexpr int MISMATCH_LIMIT = 4;
static constexpr int MAX_RESYNCS = 4;

struct Obst { float l, r, b, t; bool solid; };
struct Ev { float time; bool hold; };
struct PendEv { float remaining; bool hold; };

struct ReGuard {
	bool& flag;
	explicit ReGuard(bool& f) : flag(f) { flag = true; }
	~ReGuard() { flag = false; }
};

static std::vector<Obst> g_cache;
static float g_cacheMin = 0.f, g_cacheMax = 0.f;
static bool g_cacheValid = false;
static int g_diagLogged = 0;
static int g_rectLogged = 0;

static int windowTicks() {
	auto v = Mod::get()->getSettingValue<int64_t>("window-ticks");
	if (v < 0) v = 0;
	if (v > 64) v = 64;
	return static_cast<int>(v);
}

static int dangerTicks() {
	auto v = Mod::get()->getSettingValue<int64_t>("danger-ticks");
	if (v < 2) v = 2;
	if (v > 16) v = 16;
	return static_cast<int>(v);
}

static int minGainTicks() {
	auto v = Mod::get()->getSettingValue<int64_t>("min-gain-ticks");
	if (v < 1) v = 1;
	if (v > 16) v = 16;
	return static_cast<int>(v);
}

static bool dbg() { return Mod::get()->getSettingValue<bool>("debug-log"); }

static void showSaved(int ticks) {
	if (ticks <= 0) return;
	if (!Mod::get()->getSettingValue<bool>("show-indicator")) return;
	auto pl = PlayLayer::get();
	if (!pl) return;

	pl->removeChildByTag(SAVED_LABEL_TAG);
	auto win = CCDirector::get()->getWinSize();
	std::string text = fmt::format("Saved +{} tick{} ({:.2f}f)", ticks, ticks == 1 ? "" : "s", ticks * TICK);
	auto label = CCLabelBMFont::create(text.c_str(), "bigFont.fnt");
	label->setScale(0.45f);
	label->setPosition({win.width / 2.f, win.height - 40.f});
	label->setTag(SAVED_LABEL_TAG);
	label->setZOrder(1000);
	label->runAction(CCSequence::create(
		CCDelayTime::create(0.5f),
		CCFadeOut::create(0.4f),
		CCRemoveSelf::create(),
		nullptr
	));
	pl->addChild(label);
}

static void rebuildCache(PlayLayer* pl, float from, float to) {
	g_cache.clear();
	g_cacheValid = false;
	if (!pl || !pl->m_objects) return;
	for (auto* obj : CCArrayExt<GameObject*>(pl->m_objects)) {
		if (!obj) continue;
		auto type = obj->m_objectType;
		bool solid = (type == GameObjectType::Solid);
		bool haz = (type == GameObjectType::Hazard);
		if (!solid && !haz) continue;
		float px = obj->getPositionX();
		if (px < from - 60.f || px > to + 60.f) continue;
		CCRect r = obj->getObjectRect();
		g_cache.push_back({
			r.origin.x, r.origin.x + r.size.width,
			r.origin.y, r.origin.y + r.size.height,
			solid
		});
		if (haz && g_diagLogged < 6 && dbg()) {
			g_diagLogged++;
			log::info("diag hazard id={} rect {:.1f}x{:.1f} at ({:.0f},{:.0f}) rot={:.0f}",
				obj->m_objectID, r.size.width, r.size.height,
				r.origin.x, r.origin.y, obj->getRotation());
		}
	}
	g_cacheMin = from;
	g_cacheMax = to;
	g_cacheValid = true;
}

// vx = pixels per frame; up/down = dy/dx slopes. Returns frames survived (up to horizon).
static float simulate(float x, float y, float vx, float up, float down, float half,
                      bool startHold, const std::vector<Ev>& evs,
                      const std::vector<Obst>& obs, float horizon) {
	bool hold = startHold;
	size_t ei = 0;
	float t = 0.f;
	int steps = static_cast<int>(horizon / TICK);
	for (int i = 0; i < steps; i++) {
		while (ei < evs.size() && evs[ei].time <= t + 1e-4f) {
			hold = evs[ei].hold;
			ei++;
		}
		float slope = hold ? up : down;
		float nx = x + vx * TICK;
		float ny = y + slope * vx * TICK;
		for (auto const& o : obs) {
			if (o.r < nx - half || o.l > nx + half) continue;
			bool ovNew = !(o.t < ny - half || o.b > ny + half);
			if (!ovNew) continue;
			if (!o.solid) return t;                 // hazard = death
			bool ovOld = !(o.t < y - half || o.b > y + half);
			if (ovOld) return t;                    // hit the side of a block
			ny = y;                                 // slide along top/bottom
		}
		x = nx;
		y = ny;
		t += TICK;
	}
	return horizon;
}

class $modify(BufferPlayer, PlayerObject) {
	struct Fields {
		bool inside = false;        // true while we call the game's original code
		int eventsThisTick = 0;
		std::deque<PendEv> queue;
		int lastShift = 0;          // delay (ticks) given to the most recent queued event
		bool appliedHold = false;   // what we believe the game currently has
		int settle = 0;             // ticks since the last applied event
		int mismatch = 0;
		int resyncs = 0;
		float slopeMag = 1.f;
		float vx = 0.f;
		bool haveVx = false;
	};

	bool origPush() {
		auto f = m_fields.self();
		ReGuard g(f->inside);
		return PlayerObject::pushButton(PlayerButton::Jump);
	}

	bool origRelease() {
		auto f = m_fields.self();
		ReGuard g(f->inside);
		return PlayerObject::releaseButton(PlayerButton::Jump);
	}

	bool waveAssistActive() {
		return PlayLayer::get() && m_isDart && windowTicks() > 0
			&& Mod::get()->getSettingValue<bool>("wave-assist")
			&& m_fields->haveVx;
	}

	void applyEvent(bool hold) {
		auto f = m_fields.self();
		f->appliedHold = hold;
		f->settle = SETTLE_TICKS;
		if (hold) origPush();
		else origRelease();
	}

	void flushQueue() {
		auto f = m_fields.self();
		while (!f->queue.empty()) {
			bool h = f->queue.front().hold;
			f->queue.pop_front();
			applyEvent(h);
		}
		f->lastShift = 0;
	}

	bool passThrough(bool hold) {
		auto f = m_fields.self();
		f->appliedHold = hold;
		f->settle = SETTLE_TICKS;
		f->lastShift = 0;
		return hold ? origPush() : origRelease();
	}

	bool handleWaveEvent(bool hold) {
		auto f = m_fields.self();

		if (++f->eventsThisTick > MAX_EVENTS_PER_TICK) return passThrough(hold);

		float lastT = f->queue.empty() ? 0.f : f->queue.back().remaining;
		bool lastHold = f->queue.empty() ? f->appliedHold : f->queue.back().hold;
		// if the previous event is still pending, keep the same offset so intervals are preserved
		int prevShift = f->queue.empty() ? 0 : f->lastShift;

		if (hold == lastHold) {
			if (f->queue.empty()) return passThrough(hold);
			f->queue.push_back({lastT, hold});
			return true;
		}

		auto pl = PlayLayer::get();
		float maxA = windowTicks() * TICK;
		float need = dangerTicks() * TICK;
		float gain = minGainTicks() * TICK;
		float horizon = maxA + need + 3.f;
		float px = this->getPositionX();
		float py = this->getPositionY();
		float span = f->vx * horizon;

		if (!g_cacheValid || px - 40.f < g_cacheMin || px + span + 80.f > g_cacheMax) {
			rebuildCache(pl, px - 100.f, px + std::max(span + 80.f, 1500.f));
		}

		float mag = f->slopeMag;
		float dir = m_isUpsideDown ? -1.f : 1.f;
		float up = mag * dir;
		float down = -mag * dir;

		float reach = mag * span + 40.f;
		std::vector<Obst> local;
		for (auto const& o : g_cache) {
			if (o.r >= px - 30.f && o.l <= px + span + 40.f &&
			    o.t >= py - reach && o.b <= py + reach) {
				local.push_back(o);
			}
		}

		float scale = std::clamp(std::fabs(this->getScaleX()), 0.3f, 1.2f);
		float half = 5.f * scale;

		if (dbg() && g_rectLogged < 3) {
			g_rectLogged++;
			CCRect pr = this->getObjectRect();
			log::info("diag player rect {:.1f}x{:.1f} scale={:.2f} slope={:.0f} vx={:.2f}px/f",
				pr.size.width, pr.size.height, this->getScaleX(), mag, f->vx);
		}

		std::vector<Ev> base;
		for (auto const& e : f->queue) base.push_back({e.remaining, e.hold});

		int kmin = static_cast<int>(std::ceil(lastT / TICK - 1e-4f));
		int kmax = static_cast<int>(std::floor(maxA / TICK + 1e-4f));
		if (kmin < 0) kmin = 0;
		if (kmax < kmin) kmax = kmin;
		int n = kmax - kmin + 1;
		int k0 = std::clamp(prevShift, kmin, kmax);   // reference delay
		int i0 = k0 - kmin;

		std::vector<float> T(n, 0.f);
		for (int i = 0; i < n; i++) {
			auto evs = base;
			evs.push_back({(kmin + i) * TICK, hold});
			T[i] = simulate(px, py, f->vx, up, down, half,
			                f->appliedHold, evs, local, horizon);
		}

		// frames you survive AFTER the click is applied
		auto post = [&](int i) {
			float k = (kmin + i) * TICK;
			float t = T[i] >= horizon - 1e-4f ? horizon : T[i];
			return t - k;
		};

		float refPost = post(i0);
		float best = refPost;
		for (int i = 0; i < n; i++) best = std::max(best, post(i));

		int bi = i0;
		// only act if the original click is in danger AND a delay is clearly better
		if (refPost < need - 1e-4f && best >= refPost + gain - 1e-4f) {
			float target = std::min(need, best) - TICK;
			int found = -1;
			for (int d = 1; d < n && found < 0; d++) {
				int lo = i0 - d;
				int hi = i0 + d;
				if (lo >= 0 && post(lo) >= target) found = lo;
				else if (hi < n && post(hi) >= target) found = hi;
			}
			if (found >= 0) {
				bi = found;
				// 1 tick of margin for hitbox estimation errors
				if (found > i0 && found + 1 < n && post(found + 1) >= target) bi = found + 1;
			}
		}

		int chosen = kmin + bi;
		f->lastShift = chosen;

		if (dbg() && chosen != k0) {
			log::info("wave {} delayed {} ticks ({:.2f}f) | reference={} | after-click survival: reference {:.2f}f, best {:.2f}f, chosen {:.2f}f | obstacles={}",
				hold ? "PRESS" : "RELEASE", chosen, chosen * TICK, k0,
				refPost, best, post(bi), local.size());
		}

		if (chosen == 0 && f->queue.empty()) return passThrough(hold);

		f->queue.push_back({chosen * TICK, hold});
		showSaved(chosen);
		return true;
	}

	bool pushButton(PlayerButton button) {
		auto f = m_fields.self();
		if (f->inside || button != PlayerButton::Jump) return PlayerObject::pushButton(button);

		if (waveAssistActive()) return handleWaveEvent(true);

		f->appliedHold = true;
		f->settle = SETTLE_TICKS;
		return origPush();
	}

	bool releaseButton(PlayerButton button) {
		auto f = m_fields.self();
		if (f->inside || button != PlayerButton::Jump) return PlayerObject::releaseButton(button);

		if (waveAssistActive()) return handleWaveEvent(false);

		f->appliedHold = false;
		f->settle = SETTLE_TICKS;
		return origRelease();
	}

	void update(float dt) {
		auto f = m_fields.self();
		if (dt > 1e-5f) f->eventsThisTick = 0;

		// apply events whose delay has elapsed, before this tick's physics
		if (!f->queue.empty()) {
			if (!(PlayLayer::get() && m_isDart)) {
				flushQueue();
			} else {
				while (!f->queue.empty() && f->queue.front().remaining <= 1e-4f) {
					bool h = f->queue.front().hold;
					f->queue.pop_front();
					applyEvent(h);
				}
			}
		}

		float px = this->getPositionX();
		float py = this->getPositionY();

		PlayerObject::update(dt);

		// time passes for the remaining queued events
		for (auto& e : f->queue) e.remaining -= dt;

		if (f->settle > 0) f->settle--;

		// learn the wave's real speed (px/frame) and slope (1 normal, 2 mini),
		// and check that our idea of "holding" matches the real movement
		if (m_isDart && dt > 1e-5f) {
			float ddx = this->getPositionX() - px;
			float ddy = this->getPositionY() - py;
			if (ddx > 0.001f && ddx < 60.f) {
				f->vx = ddx / dt;
				f->haveVx = true;
				float s = ddy / ddx;
				float sa = std::fabs(s);
				if (sa > 0.9f && sa < 3.f) {
					f->slopeMag = (sa > 1.5f) ? 2.f : 1.f;

					int upSign = m_isUpsideDown ? -1 : 1;
					int dirNow = s > 0.f ? 1 : -1;
					bool obsHold = (dirNow == upSign);
					if (f->settle == 0 && f->queue.empty()) {
						if (obsHold != f->appliedHold) {
							if (++f->mismatch >= MISMATCH_LIMIT && f->resyncs < MAX_RESYNCS) {
								f->appliedHold = obsHold;
								f->mismatch = 0;
								f->resyncs++;
								if (dbg()) log::info("resync: hold state corrected to {}", obsHold ? "held" : "released");
							}
						} else {
							f->mismatch = 0;
						}
					}
				}
			}
		}
	}

	void resetObject() {
		PlayerObject::resetObject();
		auto f = m_fields.self();
		f->queue.clear();
		f->lastShift = 0;
		f->appliedHold = false;
		f->settle = 0;
		f->mismatch = 0;
		f->resyncs = 0;
		f->inside = false;
		f->eventsThisTick = 0;
		g_cacheValid = false;
	}

	void playerDestroyed(bool p0) {
		auto f = m_fields.self();
		f->queue.clear();
		f->lastShift = 0;
		f->appliedHold = false;
		PlayerObject::playerDestroyed(p0);
	}
};
