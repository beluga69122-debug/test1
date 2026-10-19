#include <Geode/Geode.hpp>
#include <Geode/modify/PlayerObject.hpp>

using namespace geode::prelude;

static constexpr int SAVED_LABEL_TAG = 0x5AFE;

static int windowFrames() {
	auto v = Mod::get()->getSettingValue<int64_t>("window-frames");
	if (v < 0) v = 0;
	if (v > 20) v = 20;
	return static_cast<int>(v);
}

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

class $modify(BufferPlayer, PlayerObject) {
	struct Fields {
		bool pending = false;
		bool userDown = false;
		bool releaseNext = false;
		float age = 0.f;
	};

	bool continuousMode() {
		return m_isShip || m_isDart || m_isSwing || m_isBird;
	}

	bool modActive() {
		return PlayLayer::get() && windowFrames() > 0 && !continuousMode();
	}

	bool canAct() {
		return m_isOnGround || !m_touchedRings.empty();
	}

	void clearState() {
		auto f = m_fields.self();
		f->pending = false;
		f->releaseNext = false;
		f->age = 0.f;
	}

	bool pushButton(PlayerButton button) {
		if (button != PlayerButton::Jump) return PlayerObject::pushButton(button);

		auto f = m_fields.self();
		f->userDown = true;
		f->releaseNext = false;
		f->pending = false;
		f->age = 0.f;

		if (modActive() && !canAct()) {
			f->pending = true;
		}
		return PlayerObject::pushButton(button);
	}

	bool releaseButton(PlayerButton button) {
		if (button == PlayerButton::Jump) {
			m_fields->userDown = false;
		}
		return PlayerObject::releaseButton(button);
	}

	void update(float dt) {
		auto f = m_fields.self();

		if (f->releaseNext) {
			f->releaseNext = false;
			if (!f->userDown) PlayerObject::releaseButton(PlayerButton::Jump);
		}

		if (f->pending) {
			if (!modActive()) {
				clearState();
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
						PlayerObject::pushButton(PlayerButton::Jump);
						if (!f->userDown) f->releaseNext = true;
					}
					showSaved(saved);
				}
			}
		}

		PlayerObject::update(dt);
	}

	void resetObject() {
		PlayerObject::resetObject();
		clearState();
		m_fields->userDown = false;
	}

	void playerDestroyed(bool p0) {
		clearState();
		PlayerObject::playerDestroyed(p0);
	}
};
