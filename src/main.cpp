#include <Geode/Geode.hpp>
#include <Geode/modify/PlayerObject.hpp>
#include <vector>
#include <deque>
#include <cmath>
#include <algorithm>

using namespace geode::prelude;

static constexpr int SAVED_LABEL_TAG = 0x5AFE;
static constexpr float SIM_DT = 1.f / 240.f;
static constexpr float HORIZON = 0.30f;      // seconds simulated ahead
static constexpr float MIN_GAIN = 0.0125f;   // must survive 3+ ticks longer to change anything

struct Obst { float l, r, b, t; bool solid; };
struct Ev { float time; bool hold; };
struct PendEv { float remaining; bool hold; };

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

// Returns how many seconds (up to horizon) the wave survives.
static float simulate(float x, float y, float vx, float up, float down, float half,
                      bool startHold, const std::vector<Ev>& evs,
                      const std::vector<Obst>& obs, float horizon) {
	bool hold = startHold;
	size_t ei = 0;
	float t = 0.f;
	int steps = static_cast<int>(horizon / SIM_DT);
	for (int i = 0; i < steps; i++) {
		while (ei < evs.size() && evs[ei].time <= t + 1e-6f) {
			hold = evs[ei].hold;
			ei++;
		}
		float slope = hold ? up : down;
		float nx = x + vx * SIM_DT;
		float ny = y + slope * vx * SIM_DT;
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
		t += SIM_DT;
	}
	return horizon;
}

class $modify(BufferPlayer, PlayerObject) {
	struct Fields {
		// wave
		std::deque<PendEv> queue;
		bool appliedHold = false;
		float upSlope = 1.f;
		float downSlope = -1.f;
		float vx = 0.f;
		bool haveVx = false;
		// ground
		bool pending = false;
		bool userDown = false;
		bool releaseNext = false;
		float age = 0.f;
	};

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
		if (hold) PlayerObject::pushButton(PlayerButton::Jump);
		else PlayerObject::releaseButton(PlayerButton::Jump);
	}

	void flushQueue() {
		auto f = m_fields.self();
		while (!f->queue.empty()) {
			bool h = f->queue.front().hold;
			f->queue.pop_front();
			applyEvent(h);
		}
	}

	bool handleWaveEvent(bool hold) {
		auto f = m_fields.self();
		float last = f->queue.empty() ? 0.f : f->queue.back().remaining;
		bool lastHold = f->queue.empty() ? f->appliedHold : f->queue.back().hold;

		// duplicate state: nothing to decide
		if (hold == lastHold) {
			if (f->queue.empty()) {
				f->appliedHold = hold;
				return hold ? PlayerObject::pushButton(PlayerButton::Jump)
				            : PlayerObject::releaseButton(PlayerButton::Jump);
			}
			f->queue.push_back({last, hold});
			return true;
		}

		auto pl = PlayLayer::get();
		float maxA = static_cast<float>(windowFrames()) / 60.f;
		float px = this->getPositionX();
		float py = this->getPositionY();
		float span = f->vx * (HORIZON + maxA);

		if (!g_cacheValid || px - 40.f < g_cacheMin || px + span + 80.f > g_cacheMax) {
			rebuildCache(pl, px - 100.f, px + std::max(span + 80.f, 1500.f));
		}

		float reach = std::max(std::fabs(f->upSlope), std::fabs(f->downSlope)) * span + 40.f;
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

		int kmin = static_cast<int>(std::ceil(last / SIM_DT - 1e-4f));
		int kmax = static_cast<int>(std::floor(maxA / SIM_DT + 1e-4f));
		if (kmin < 0) kmin = 0;
		if (kmax < kmin) kmax = kmin;

		std::vector<float> T(kmax - kmin + 1, 0.f);
		float Tmax = -1.f;
		for (int k = kmin; k <= kmax; k++) {
			auto evs = base;
			evs.push_back({k * SIM_DT, hold});
			float t = simulate(px, py, f->vx, f->upSlope, f->downSlope, half,
			                   f->appliedHold, evs, local, HORIZON);
			T[k - kmin] = t;
			if (t > Tmax) Tmax = t;
		}

		int chosen = kmin;
		if (Tmax > T[0] + MIN_GAIN) {
			for (int k = kmin; k <= kmax; k++) {
				if (T[k - kmin] >= Tmax - 1e-5f) { chosen = k; break; }
			}
		}

		float a = chosen * SIM_DT;
		int frames = static_cast<int>(std::lround(a * 60.f));

		if (dbg()) {
			log::info("wave {} | obstacles={} | survive(no delay)={:.3f}s best={:.3f}s | delay={}f | up={:.2f} down={:.2f} vx={:.1f}",
				hold ? "PRESS" : "RELEASE", local.size(), T[0], Tmax, frames,
				f->upSlope, f->downSlope, f->vx);
		}

		if (a <= 1e-6f && f->queue.empty()) {
			f->appliedHold = hold;
			return hold ? PlayerObject::pushButton(PlayerButton::Jump)
			            : PlayerObject::releaseButton(PlayerButton::Jump);
		}

		f->queue.push_back({a, hold});
		if (frames > 0) showSaved(frames);
		return true;
	}

	// ---------- hooks ----------
	bool pushButton(PlayerButton button) {
		if (button != PlayerButton::Jump) return PlayerObject::pushButton(button);

		auto f = m_fields.self();
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
		return PlayerObject::pushButton(button);
	}

	bool releaseButton(PlayerButton button) {
		if (button != PlayerButton::Jump) return PlayerObject::releaseButton(button);

		auto f = m_fields.self();
		f->userDown = false;

		if (waveAssistActive()) return handleWaveEvent(false);

		f->appliedHold = false;
		return PlayerObject::releaseButton(button);
	}

	void update(float dt) {
		auto f = m_fields.self();

		// --- wave: apply delayed events ---
		if (!f->queue.empty()) {
			if (!(PlayLayer::get() && m_isDart)) {
				flushQueue();
			} else {
				for (auto& e : f->queue) e.remaining -= dt;
				while (!f->queue.empty() && f->queue.front().remaining <= 1e-6f) {
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
				PlayerObject::releaseButton(PlayerButton::Jump);
			}
		}
		if (f->pending) {
			if (!groundActive()) {
				clearGround();
			} else {
				f->age += dt;
				float limit = static_cast<float>(windowFrames()) / 60.f;
				if (f->age > limit + 0.0001f) {
					f->pending = false;
				} else if (canAct()) {
					int saved = static_cast<int>(std::lround(f->age * 60.f));
					f->pending = false;
					bool vanillaHandles = f->userDown && m_isOnGround && m_touchedRings.empty();
					if (!vanillaHandles) {
						f->appliedHold = true;
						PlayerObject::pushButton(PlayerButton::Jump);
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

		// --- wave: learn real speed and slopes ---
		if (m_isDart && dt > 0.00001f) {
			float ddx = this->getPositionX() - px;
			float ddy = this->getPositionY() - py;
			if (ddx > 0.01f && ddx < 50.f) {
				float v = ddx / dt;
				float s = ddy / ddx;
				if (v > 1.f) {
					f->vx = v;
					f->haveVx = true;
				}
				if (std::fabs(s) > 0.5f && std::fabs(s) < 4.f) {
					if (f->appliedHold) f->upSlope = s;
					else f->downSlope = s;
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
