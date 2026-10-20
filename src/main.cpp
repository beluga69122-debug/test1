#include <Geode/Geode.hpp>
#include <Geode/modify/PlayerObject.hpp>
#include <vector>
#include <deque>
#include <cmath>
#include <algorithm>

using namespace geode::prelude;

static constexpr int SAVED_LABEL_TAG = 0x5AFE;

// All times below are in "frames" (1 frame = 1/60 s).
static constexpr float STEP = 0.25f;     // simulation step (frames)
static constexpr float TAIL = 14.f;      // frames simulated after the buffer window
static constexpr float MIN_GAIN = 0.75f; // must survive 3+ steps longer to change a click
static constexpr int MAX_EVENTS_PER_TICK = 6;

struct Obst { float l, r, b, t; bool solid; };
struct Ev { float time; bool hold; };
struct PendEv { float remaining; bool hold; };

// Sets a flag while we call into the game's original button code.
struct ReGuard {
	bool& flag;
	explicit ReGuard(bool& f) : flag(f) { flag = true; }
	~ReGuard() { flag = false; }
};

static std::vector<Obst> g_cache;
static float g_cacheMin = 0.f, g_cacheMax = 0.f;
static bool g_cacheValid = false;

static int windowFrames() {
	auto v = Mod::get()->getSettingValue<int64_t>("window-frames");
	if (v < 0) v = 0;
	if (v > 20) v = 20;
	return static_cast<int>(v);
}

static bool dbg() { return Mod::get()->getSettingValue<bool>("debug-log"); }

static void showSaved(int frames) {
	if (frames <= 0) return;
	if (!Mod::get()->getSettingValue<bool>("show-indicator")) return;
	auto pl = PlayLayer::get();
	if (!pl) return;

	pl->removeChildByTag(SAVED_LABEL_TAG);
	auto win = CCDirector::get()->getWinSize();
	std::string text = "Saved " + std::to_string(frames) + (frames == 1 ? " frame" : " frames");
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
	}
	g_cacheMin = from;
	g_cacheMax = to;
	g_cacheValid = true;
}

// vx = pixels per frame, up/down = dy/dx slopes. Returns frames survived (up to horizon).
static float simulate(float x, float y, float vx, float up, float down, float half,
                      bool startHold, const std::vector<Ev>& evs,
                      const std::vector<Obst>& obs, float horizon) {
	bool hold = startHold;
	size_t ei = 0;
	float t = 0.f;
	int steps = static_cast<int>(horizon / STEP);
	for (int i = 0; i < steps; i++) {
		while (ei < evs.size() && evs[ei].time <= t + 1e-4f) {
			hold = evs[ei].hold;
			ei++;
		}
		float slope = hold ? up : down;
		float nx = x + vx * STEP;
		float ny = y + slope * vx * STEP;
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
		t += STEP;
	}
	return horizon;
}

class $modify(BufferPlayer, PlayerObject) {
	struct Fields {
		// shared
		bool inside = false;        // true while we are calling the game's original code
		int eventsThisTick = 0;
		// wave
		std::deque<PendEv> queue;
		bool appliedHold = false;
		float slopeMag = 1.f;
		float vx = 0.f;
		bool haveVx = false;
		// ground
		bool pending = false;
		bool userDown = false;
		bool releaseNext = false;
		float age = 0.f;
	};

	// ---------- calls into the game's original code (re-entrancy safe) ----------
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

	// ---------- helpers ----------
	bool waveAssistActive() {
		return PlayLayer::get() && m_isDart && windowFrames() > 0
			&& Mod::get()->getSettingValue<bool>("wave-assist")
			&& m_fields->haveVx;
	}

	bool groundModeOk() {
		return !(m_isShip || m_isDart || m_isSwing || m_isBird);
	}

	bool groundActive() {
		return PlayLayer::get() && windowFrames() > 0 && groundModeOk()
			&& Mod::get()->getSettingValue<bool>("ground-assist");
	}

	bool canAct() {
		return m_isOnGround || !m_touchedRings.empty();
	}

	void clearGround() {
		auto f = m_fields.self();
		f->pending = false;
		f->releaseNext = false;
		f->age = 0.f;
	}

	void applyEvent(bool hold) {
		m_fields->appliedHold = hold;
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
	}

	bool passThrough(bool hold) {
		m_fields->appliedHold = hold;
		return hold ? origPush() : origRelease();
	}

	bool handleWaveEvent(bool hold) {
		auto f = m_fields.self();

		// safety: never process a runaway number of events in the same tick
		if (++f->eventsThisTick > MAX_EVENTS_PER_TICK) return passThrough(hold);

		float last = f->queue.empty() ? 0.f : f->queue.back().remaining;
		bool lastHold = f->queue.empty() ? f->appliedHold : f->queue.back().hold;

		// same state as before: nothing to decide
		if (hold == lastHold) {
			if (f->queue.empty()) return passThrough(hold);
			f->queue.push_back({last, hold});
			return true;
		}

		auto pl = PlayLayer::get();
		float maxA = static_cast<float>(windowFrames());
		float horizon = std::min(maxA + TAIL, 40.f);
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

		std::vector<Ev> base;
		for (auto const& e : f->queue) base.push_back({e.remaining, e.hold});

		int kmin = static_cast<int>(std::ceil(last / STEP - 1e-4f));
		int kmax = static_cast<int>(std::floor(maxA / STEP + 1e-4f));
		if (kmin < 0) kmin = 0;
		if (kmax < kmin) kmax = kmin;

		std::vector<float> T(kmax - kmin + 1, 0.f);
		float Tmax = -1.f;
		for (int k = kmin; k <= kmax; k++) {
			auto evs = base;
			evs.push_back({k * STEP, hold});
			float t = simulate(px, py, f->vx, up, down, half,
			                   f->appliedHold, evs, local, horizon);
			T[k - kmin] = t;
			if (t > Tmax) Tmax = t;
		}

		int chosen = kmin;
		if (Tmax > T[0] + MIN_GAIN) {
			for (int k = kmin; k <= kmax; k++) {
				if (T[k - kmin] >= Tmax - 1e-4f) { chosen = k; break; }
			}
		}

		float a = chosen * STEP;
		int frames = static_cast<int>(std::lround(a));

		if (dbg() && chosen > kmin) {
			log::info("wave {} delayed {:.2f}f | survive(no delay)={:.2f}f best={:.2f}f of {:.0f}f | obstacles={}",
				hold ? "PRESS" : "RELEASE", a, T[0], Tmax, horizon, local.size());
		}

		if (a <= 1e-6f && f->queue.empty()) return passThrough(hold);

		f->queue.push_back({a, hold});
		if (frames > 0) showSaved(frames);
		return true;
	}

	// ---------- hooks ----------
	bool pushButton(PlayerButton button) {
		auto f = m_fields.self();
		// re-entrant call (another mod / the game calling us back): do not touch it
		if (f->inside || button != PlayerButton::Jump) return PlayerObject::pushButton(button);

		f->userDown = true;

		if (waveAssistActive()) return handleWaveEvent(true);

		f->appliedHold = true;
		f->releaseNext = false;
		f->pending = false;
		f->age = 0.f;

		if (groundActive() && !canAct()) {
			f->pending = true;
			if (dbg()) log::info("ground: early click stored");
		}
		return origPush();
	}

	bool releaseButton(PlayerButton button) {
		auto f = m_fields.self();
		if (f->inside || button != PlayerButton::Jump) return PlayerObject::releaseButton(button);

		f->userDown = false;

		if (waveAssistActive()) return handleWaveEvent(false);

		f->appliedHold = false;
		return origRelease();
	}

	void update(float dt) {
		auto f = m_fields.self();
		if (dt > 1e-5f) f->eventsThisTick = 0;

		// --- wave: apply delayed events (dt is in frames) ---
		if (!f->queue.empty()) {
			if (!(PlayLayer::get() && m_isDart)) {
				flushQueue();
			} else {
				for (auto& e : f->queue) e.remaining -= dt;
				while (!f->queue.empty() && f->queue.front().remaining <= 1e-4f) {
					bool h = f->queue.front().hold;
					f->queue.pop_front();
					applyEvent(h);
				}
			}
		}

		// --- ground: replay early click ---
		if (f->releaseNext) {
			f->releaseNext = false;
			if (!f->userDown) {
				f->appliedHold = false;
				origRelease();
			}
		}
		if (f->pending) {
			if (!groundActive()) {
				clearGround();
			} else {
				f->age += dt;
				float limit = static_cast<float>(windowFrames());
				if (f->age > limit + 0.001f) {
					f->pending = false;
				} else if (canAct()) {
					int saved = static_cast<int>(std::lround(f->age));
					f->pending = false;
					bool vanillaHandles = f->userDown && m_isOnGround && m_touchedRings.empty();
					if (!vanillaHandles) {
						f->appliedHold = true;
						origPush();
						if (!f->userDown) f->releaseNext = true;
					}
					if (dbg()) log::info("ground: applied early click, saved {} frames", saved);
					showSaved(saved);
				}
			}
		}

		float px = this->getPositionX();
		float py = this->getPositionY();

		PlayerObject::update(dt);

		// --- wave: learn real speed (px/frame) and slope magnitude ---
		if (m_isDart && dt > 1e-5f) {
			float ddx = this->getPositionX() - px;
			float ddy = this->getPositionY() - py;
			if (ddx > 0.001f && ddx < 60.f) {
				f->vx = ddx / dt;
				f->haveVx = true;
				float s = std::fabs(ddy / ddx);
				if (s > 0.9f && s < 3.f) {
					f->slopeMag = (s > 1.5f) ? 2.f : 1.f;
				}
			}
		}
	}

	void resetObject() {
		PlayerObject::resetObject();
		auto f = m_fields.self();
		f->queue.clear();
		f->appliedHold = false;
		f->userDown = false;
		f->inside = false;
		f->eventsThisTick = 0;
		clearGround();
		g_cacheValid = false;
	}

	void playerDestroyed(bool p0) {
		auto f = m_fields.self();
		f->queue.clear();
		f->appliedHold = false;
		clearGround();
		PlayerObject::playerDestroyed(p0);
	}
};
