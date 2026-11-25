#include "Output.hpp"
#include "RunRecorder.hpp"
#include "Settings.hpp"

#include <Geode/Geode.hpp>
#include <Geode/modify/CCEGLView.hpp>
#include <Geode/modify/EndLevelLayer.hpp>
#include <Geode/modify/PlayLayer.hpp>

#include <algorithm>
#include <cmath>
#include <ctime>
#include <limits>
#include <vector>

using namespace geode::prelude;

// Remove half-written attempt files left behind by a crash or by closing the game mid-save.
$execute {
    rgd::cleanupTempDir();
}

namespace {

// With practice checkpoints (or platformer checkpoints), a death respawns you mid-run:
// that's still the same run, so it must not throw the recording away.
bool hasCheckpoints(PlayLayer* pl) {
    return pl->m_checkpointArray && pl->m_checkpointArray->count() > 0;
}

bool shouldRecord(PlayLayer* pl) {
    auto const s = rgd::RecordSettings::load();
    if (!s.enabled) return false;
    if (pl->m_isPracticeMode && !s.recordPractice) return false;
    if (pl->m_isTestMode && !s.recordStartPos) return false; // started from a start position
    return true;
}

rgd::LevelMeta makeMeta(PlayLayer* pl) {
    rgd::LevelMeta meta;
    if (pl->m_level) {
        meta.levelName = pl->m_level->m_levelName.c_str();
        meta.levelID = pl->m_level->m_levelID.value();
    }
    meta.attempts = pl->m_attempts;
    meta.practice = pl->m_isPracticeMode;
    meta.startPos = pl->m_isTestMode;
    meta.completedAt = std::time(nullptr);
    return meta;
}

} // namespace

class $modify(RGDPlayLayer, PlayLayer) {
    // First attempt after entering the level.
    void startGame() {
        PlayLayer::startGame();
        auto& rec = rgd::RunRecorder::get();
        if (!rec.isActive() && !hasCheckpoints(this) && shouldRecord(this)) rec.start();
    }

    // Every respawn / restart goes through here.
    void resetLevel() {
        PlayLayer::resetLevel();

        auto& rec = rgd::RunRecorder::get();
        if (rec.inTail()) rec.finalize(); // "Replay" pressed on the end screen: save what we have

        if (!hasCheckpoints(this)) {
            // Fresh attempt from the start. The recording session keeps running (so the end of the
            // previous attempt can be used as pre-roll); start() marks where this attempt begins.
            if (shouldRecord(this)) {
                rec.start(); // also drops an unanswered end-screen run
            } else {
                rec.discard();
                rec.abandonPending();
            }
        }
        // Otherwise we respawned at a checkpoint: keep recording the same run.
    }

    // Deaths are not hooked: the session keeps recording through the death and respawn, so they can
    // appear as pre-roll before the winning attempt. resetLevel() marks the next attempt.

    void togglePracticeMode(bool practiceMode) {
        PlayLayer::togglePracticeMode(practiceMode);
        if (practiceMode && !rgd::RecordSettings::load().recordPractice) {
            rgd::RunRecorder::get().discard();
        }
    }

    void levelComplete() {
        auto& rec = rgd::RunRecorder::get();
        bool keep = rec.isRecording();

        // Check before the original call, which marks the level as completed.
        if (keep && rgd::RecordSettings::load().firstCompletionOnly &&
            GameStatsManager::sharedState()->hasCompletedLevel(m_level)) {
            rec.discard();
            keep = false;
        }

        PlayLayer::levelComplete();

        if (keep) rec.complete(makeMeta(this));
    }

    void onQuit() {
        // Auto mode: a completed run is still saved. Ask mode: an unanswered run is dropped.
        rgd::RunRecorder::get().onLevelExit();
        PlayLayer::onQuit();
    }
};

// "Save Run" button on the end screen (only with "Ask Before Saving" on).
namespace {

constexpr int kPulseTag = 0x52474450; // "RGDP"

bool isBigButton(CCNode* node) {
    return typeinfo_cast<CCMenuItemSpriteExtra*>(node) && node->isVisible() && node->getScaledContentSize().height >= 30.f;
}

// The end screen has no node IDs in Geode 6, so find its row of big buttons (Replay, Menu, Edit...)
// as the menu in the layer whose large buttons sit lowest on screen.
CCMenu* findBottomButtonMenu(CCNode* layer) {
    CCMenu* best = nullptr;
    float bestY = std::numeric_limits<float>::max();
    for (auto child : CCArrayExt<CCNode*>(layer->getChildren())) {
        auto menu = typeinfo_cast<CCMenu*>(child);
        if (!menu || !menu->isVisible()) continue;
        int count = 0;
        float sumY = 0.f;
        for (auto item : CCArrayExt<CCNode*>(menu->getChildren())) {
            if (!isBigButton(item)) continue;
            ++count;
            sumY += menu->convertToWorldSpace(item->getPosition()).y;
        }
        if (count < 2) continue;
        if (float const avg = sumY / count; avg < bestY) {
            bestY = avg;
            best = menu;
        }
    }
    return best;
}

CircleButtonSprite* makeSaveSprite(char const* icon, CircleBaseColor color) {
    return CircleButtonSprite::createWithSprite(icon, 1.f, color, CircleBaseSize::Big);
}

} // namespace

class $modify(RGDEndLevelLayer, EndLevelLayer) {
    struct Fields {
        CCMenuItemSpriteExtra* saveButton = nullptr;
        float spriteScale = 1.f;
    };

    void customSetup() {
        EndLevelLayer::customSetup();
        if (!rgd::RunRecorder::get().awaitingDecision()) return;

        auto sprite = makeSaveSprite("save-icon.png"_spr, CircleBaseColor::Green);
        if (!sprite) return;

        if (auto row = findBottomButtonMenu(m_mainLayer)) {
            this->addToButtonRow(row, sprite);
        } else {
            // Fallback: top-right corner, clear of the end screen's own buttons.
            m_fields->spriteScale = .8f;
            sprite->setScale(m_fields->spriteScale);
            auto menu = CCMenu::create();
            menu->setID("save-run-menu"_spr);
            auto const win = CCDirector::get()->getWinSize();
            menu->setPosition({win.width - 45.f, win.height - 45.f});
            m_mainLayer->addChild(menu, 10);
            this->createSaveButton(menu, sprite, {0.f, 0.f});
        }
        this->startPulse();
    }

    // Adds the button at the end of the big-button row, same size as its neighbours, and re-centres the row.
    void addToButtonRow(CCMenu* row, CircleButtonSprite* sprite) {
        std::vector<CCNode*> items;
        for (auto item : CCArrayExt<CCNode*>(row->getChildren())) {
            if (isBigButton(item)) items.push_back(item);
        }
        std::sort(items.begin(), items.end(), [](CCNode* a, CCNode* b) { return a->getPositionX() < b->getPositionX(); });

        float height = 0.f;
        for (auto item : items) height = std::max(height, item->getScaledContentSize().height);
        float spacing = height * 1.25f;
        if (items.size() >= 2) {
            spacing = (items.back()->getPositionX() - items.front()->getPositionX()) / static_cast<float>(items.size() - 1);
        }

        // Match the neighbours' size (GD's round buttons are slightly smaller than the sprite itself).
        m_fields->spriteScale = height * .92f / sprite->getContentSize().height;
        sprite->setScale(m_fields->spriteScale);

        CCPoint const pos{items.back()->getPositionX() + spacing, items.back()->getPositionY()};
        auto button = this->createSaveButton(row, sprite, pos);

        if (row->getLayout()) {
            row->updateLayout();
        } else {
            // Shift the whole row left by half a slot so it stays centred.
            for (auto item : items) item->setPositionX(item->getPositionX() - spacing / 2.f);
            button->setPositionX(button->getPositionX() - spacing / 2.f);
        }
    }

    CCMenuItemSpriteExtra* createSaveButton(CCMenu* menu, CCSprite* sprite, CCPoint pos) {
        auto button = CCMenuItemSpriteExtra::create(sprite, this, menu_selector(RGDEndLevelLayer::onRecordGDSave));
        button->setID("save-run-button"_spr);
        button->setPosition(pos);
        menu->addChild(button);
        m_fields->saveButton = button;
        return button;
    }

    // Gentle "breathing" while the run is waiting for a decision.
    void startPulse() {
        auto sprite = m_fields->saveButton ? m_fields->saveButton->getNormalImage() : nullptr;
        if (!sprite) return;
        float const s = m_fields->spriteScale;
        auto pulse = CCRepeatForever::create(CCSequence::create(
            CCEaseSineInOut::create(CCScaleTo::create(.7f, s * 1.07f)),
            CCEaseSineInOut::create(CCScaleTo::create(.7f, s)),
            nullptr
        ));
        pulse->setTag(kPulseTag);
        sprite->runAction(pulse);
    }

    void showDecision(bool saved) {
        auto button = m_fields->saveButton;
        if (!button) return;
        auto sprite = saved ? makeSaveSprite("saved-icon.png"_spr, CircleBaseColor::Green)
                            : makeSaveSprite("save-icon.png"_spr, CircleBaseColor::Gray);
        if (!sprite) return;
        sprite->setScale(m_fields->spriteScale);
        button->setSprite(sprite); // the old sprite (and its pulse) goes away with it
        button->setEnabled(false);
    }

    void onRecordGDSave(CCObject*) {
        auto& rec = rgd::RunRecorder::get();
        if (!rec.awaitingDecision()) {
            this->showDecision(false);
            return;
        }
        // Stop recording now so the popup itself doesn't end up in the video.
        rec.stopForDecision();

        auto const& meta = rec.pendingMeta();
        int const seconds = static_cast<int>(std::lround(meta.runSeconds));
        auto content = fmt::format(
            "Save a video of this run?\n<cy>{}</c> attempt{}, <cg>{}:{:02}</c> long",
            meta.attempts, meta.attempts == 1 ? "" : "s", seconds / 60, seconds % 60
        );

        Ref<RGDEndLevelLayer> self = this;
        createQuickPopup(
            "Save Run", content, "Discard", "Save",
            [self](FLAlertLayer*, bool save) {
                rgd::RunRecorder::get().decide(save);
                self->showDecision(save);
            },
            true, true // show now; Escape closes without deciding
        );
    }
};

// The frame is finished but not yet on screen: the ideal moment to read it.
class $modify(RGDEGLView, CCEGLView) {
    void swapBuffers() {
        rgd::RunRecorder::get().onSwap();
        CCEGLView::swapBuffers();
    }
};
