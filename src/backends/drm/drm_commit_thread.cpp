/*
    KWin - the KDE window manager
    This file is part of the KDE project.

    SPDX-FileCopyrightText: 2023 Xaver Hugl <xaver.hugl@gmail.com>

    SPDX-License-Identifier: GPL-2.0-or-later
*/
#include "drm_commit_thread.h"
#include "drm_commit.h"
#include "drm_gpu.h"
#include "logging_p.h"
#include "utils/realtime.h"

using namespace std::chrono_literals;

namespace KWin
{

// This value was chosen experimentally and should be adjusted if needed
// committing takes about 800µs, the rest is accounting for sleep not being accurate enough
static constexpr auto s_safetyMargin = 1800us;

DrmCommitThread::DrmCommitThread()
{
    m_thread.reset(QThread::create([this]() {
        gainRealTime();
        while (true) {
            if (QThread::currentThread()->isInterruptionRequested()) {
                return;
            }
            std::unique_lock lock(m_mutex);
            if (m_commits.empty()) {
                m_commitPending.wait(lock);
            }
            if (m_pageflipPending) {
                // the commit would fail with EBUSY, wait until the pageflip is done
                continue;
            }
            if (!m_commits.empty()) {
                const auto now = std::chrono::steady_clock::now();
                if (m_targetPageflipTime > now + s_safetyMargin) {
                    lock.unlock();
                    std::this_thread::sleep_until(m_targetPageflipTime - s_safetyMargin);
                    lock.lock();
                }
                optimizeCommits();
                auto &commit = m_commits.front();
                if (!commit->areBuffersReadable()) {
                    // no commit is ready yet, reschedule
                    if (m_vrr) {
                        m_targetPageflipTime += 50us;
                    } else {
                        m_targetPageflipTime += m_minVblankInterval;
                    }
                    continue;
                }
                const auto vrr = commit->isVrr();
                const bool success = commit->commit();
                if (success) {
                    m_pageflipPending = true;
                    m_vrr = vrr.value_or(m_vrr);
                    // the atomic commit takes ownership of the object
                    commit.release();
                    m_commits.erase(m_commits.begin());
                } else {
                    for (auto &commit : m_commits) {
                        m_droppedCommits.push_back(std::move(commit));
                    }
                    m_commits.clear();
                    qCWarning(KWIN_DRM) << "atomic commit failed:" << strerror(errno);
                    QMetaObject::invokeMethod(this, &DrmCommitThread::commitFailed, Qt::ConnectionType::QueuedConnection);
                }
                QMetaObject::invokeMethod(this, &DrmCommitThread::clearDroppedCommits, Qt::ConnectionType::QueuedConnection);
            }
        }
    }));
    m_thread->start();
}

void DrmCommitThread::optimizeCommits()
{
    if (m_commits.size() <= 1) {
        return;
    }
    // merge commits in the front that are already ready (regardless of which planes they modify)
    if (m_commits.front()->areBuffersReadable()) {
        auto it = m_commits.begin() + 1;
        while (it != m_commits.end() && (*it)->areBuffersReadable()) {
            m_commits.front()->merge(it->get());
            m_droppedCommits.push_back(std::move(*it));
            it = m_commits.erase(it);
        }
    }
    // merge commits that are ready and modify the same drm planes
    for (auto it = m_commits.begin(); it != m_commits.end();) {
        DrmAtomicCommit *const commit = it->get();
        it++;
        while (it != m_commits.end() && commit->modifiedPlanes() == (*it)->modifiedPlanes() && (*it)->areBuffersReadable()) {
            commit->merge(it->get());
            m_droppedCommits.push_back(std::move(*it));
            it = m_commits.erase(it);
        }
    }
    if (m_commits.size() == 1) {
        // already done
        return;
    }
    std::unique_ptr<DrmAtomicCommit> front;
    if (m_commits.front()->areBuffersReadable()) {
        front = std::move(m_commits.front());
        m_commits.erase(m_commits.begin());
    }
    // try to move commits that are ready to the front
    for (auto it = m_commits.begin() + 1; it != m_commits.end();) {
        auto &commit = *it;
        // commits that target the same plane(s) need to stay in the same order
        const auto &planes = commit->modifiedPlanes();
        const bool skipping = std::any_of(m_commits.begin(), it, [&planes](const auto &other) {
            return std::any_of(planes.begin(), planes.end(), [&other](DrmPlane *plane) {
                return other->modifiedPlanes().contains(plane);
            });
        });
        if (skipping || !commit->areBuffersReadable()) {
            it++;
            continue;
        }
        // find out if the modified commit order will actually work
        std::unique_ptr<DrmAtomicCommit> duplicate;
        if (front) {
            duplicate = std::make_unique<DrmAtomicCommit>(*front);
            duplicate->merge(commit.get());
            if (!duplicate->test()) {
                m_droppedCommits.push_back(std::move(duplicate));
                it++;
                continue;
            }
        } else {
            if (!commit->test()) {
                it++;
                continue;
            }
            duplicate = std::make_unique<DrmAtomicCommit>(*commit);
        }
        bool success = true;
        for (const auto &otherCommit : m_commits) {
            if (otherCommit != commit) {
                duplicate->merge(otherCommit.get());
                if (!duplicate->test()) {
                    success = false;
                    break;
                }
            }
        }
        m_droppedCommits.push_back(std::move(duplicate));
        if (success) {
            if (front) {
                front->merge(commit.get());
                m_droppedCommits.push_back(std::move(commit));
            } else {
                front = std::move(commit);
            }
            it = m_commits.erase(it);
        } else {
            it++;
        }
    }
    if (front) {
        m_commits.insert(m_commits.begin(), std::move(front));
    }
}

DrmCommitThread::~DrmCommitThread()
{
    m_thread->requestInterruption();
    m_commitPending.notify_all();
    m_thread->wait();
}

void DrmCommitThread::addCommit(std::unique_ptr<DrmAtomicCommit> &&commit)
{
    std::unique_lock lock(m_mutex);
    m_commits.push_back(std::move(commit));
    const auto now = std::chrono::steady_clock::now();
    if (m_vrr && now >= m_lastPageflip + m_minVblankInterval) {
        m_targetPageflipTime = now;
    } else {
        m_targetPageflipTime = estimateNextVblank(now);
    }
    m_commitPending.notify_all();
}

void DrmCommitThread::clearDroppedCommits()
{
    std::unique_lock lock(m_mutex);
    m_droppedCommits.clear();
}

void DrmCommitThread::setRefreshRate(uint32_t maximum)
{
    std::unique_lock lock(m_mutex);
    m_minVblankInterval = std::chrono::nanoseconds(1'000'000'000'000ull / maximum);
}

void DrmCommitThread::pageFlipped(std::chrono::nanoseconds timestamp)
{
    std::unique_lock lock(m_mutex);
    m_lastPageflip = TimePoint(timestamp);
    m_pageflipPending = false;
    if (!m_commits.empty()) {
        m_targetPageflipTime = estimateNextVblank(std::chrono::steady_clock::now());
        m_commitPending.notify_all();
    }
}

bool DrmCommitThread::pageflipsPending()
{
    std::unique_lock lock(m_mutex);
    return !m_commits.empty() || m_pageflipPending;
}

TimePoint DrmCommitThread::estimateNextVblank(TimePoint now) const
{
    const uint64_t pageflipsSince = (now - m_lastPageflip) / m_minVblankInterval;
    return m_lastPageflip + m_minVblankInterval * (pageflipsSince + 1);
}
}
