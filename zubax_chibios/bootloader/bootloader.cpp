/*
 * Copyright (c) 2016 Zubax, zubax.com
 * Distributed under the MIT License, available in the file LICENSE.
 * Author: Pavel Kirienko <pavel.kirienko@zubax.com>
 */

#include "bootloader.hpp"
#include <ch.hpp>


namespace bootloader
{
/*
 * Bootloader
 */
std::pair<Bootloader::AppDescriptor, bool> Bootloader::locateAppDescriptor()
{
    constexpr auto Step = 8;

    for (std::size_t offset = 0;; offset += Step)
    {
        // Reading the storage in 8 bytes increments until we've found the signature
        {
            std::uint8_t signature[Step] = {};
            int res = backend_.read(offset, signature, sizeof(signature));
            if (res != sizeof(signature))
            {
                break;
            }
            const auto reference = AppDescriptor::getSignatureValue();
            if (!std::equal(std::begin(signature), std::end(signature), std::begin(reference)))
            {
                continue;
            }
        }

        // Reading the entire descriptor
        AppDescriptor desc;
        {
            int res = backend_.read(offset, &desc, sizeof(desc));
            if (res != sizeof(desc))
            {
                break;
            }
            if (!desc.isValid())
            {
                continue;
            }
        }

        // Checking firmware CRC
        {
            constexpr auto WordSize = 4;
            const auto crc_offset_in_words = (offset + offsetof(AppDescriptor, app_info.image_crc)) / WordSize;
            const auto image_size_in_words = desc.app_info.image_size / WordSize;

            CRC64WE crc;

            for (unsigned i = 0; i < image_size_in_words; i++)
            {
                std::uint32_t word = 0;
                if ((i != crc_offset_in_words) && (i != (crc_offset_in_words + 1)))
                {
                    int res = backend_.read(i * WordSize, &word, WordSize);
                    if (res != WordSize)
                    {
                        continue;
                    }
                }

                crc.add(&word, WordSize);
            }

            if (crc.get() != desc.app_info.image_crc)
            {
                DEBUG_LOG("App descriptor found, but CRC is invalid (%s != %s)\n",
                          os::uintToString(crc.get()).c_str(),
                          os::uintToString(desc.app_info.image_crc).c_str());
                continue;       // Look further...
            }
        }

        // Returning if the descriptor is correct
        DEBUG_LOG("App descriptor located at offset %x\n", unsigned(offset));
        return {desc, true};
    }

    return {AppDescriptor(), false};
}

void Bootloader::verifyAppAndUpdateState()
{
    const auto appdesc_result = locateAppDescriptor();
    if (appdesc_result.second)
    {
        DEBUG_LOG("App found; version %d.%d.%x, %d bytes\n",
                  appdesc_result.first.app_info.major_version,
                  appdesc_result.first.app_info.minor_version,
                  unsigned(appdesc_result.first.app_info.vcs_commit),
                  unsigned(appdesc_result.first.app_info.image_size));
        state_ = State::BootDelay;
        boot_delay_started_at_st_ = chVTGetSystemTime();
    }
    else
    {
        DEBUG_LOG("App not found\n");
        state_ = State::NoAppToBoot;
    }
}

Bootloader::Bootloader(IAppStorageBackend& backend, unsigned boot_delay_msec) :
    backend_(backend),
    boot_delay_msec_(boot_delay_msec)
{
    os::MutexLocker mlock(mutex_);
    verifyAppAndUpdateState();
}

State Bootloader::getState()
{
    os::MutexLocker mlock(mutex_);

    if ((state_ == State::BootDelay) && (chVTTimeElapsedSinceX(boot_delay_started_at_st_) >= MS2ST(boot_delay_msec_)))
    {
        DEBUG_LOG("Boot delay expired\n");
        state_ = State::ReadyToBoot;
    }

    return state_;
}

std::pair<AppInfo, bool> Bootloader::getAppInfo()
{
    os::MutexLocker mlock(mutex_);
    const auto res = locateAppDescriptor();
    return {res.first.app_info, res.second};
}

void Bootloader::cancelBoot()
{
    os::MutexLocker mlock(mutex_);

    switch (state_)
    {
    case State::BootDelay:
    case State::ReadyToBoot:
    {
        state_ = State::BootCancelled;
        DEBUG_LOG("Boot cancelled\n");
        break;
    }
    case State::NoAppToBoot:
    case State::BootCancelled:
    case State::AppUpgradeInProgress:
    {
        break;
    }
    }
}

void Bootloader::requestBoot()
{
    os::MutexLocker mlock(mutex_);

    switch (state_)
    {
    case State::BootDelay:
    case State::BootCancelled:
    {
        state_ = State::ReadyToBoot;
        DEBUG_LOG("Boot requested\n");
        break;
    }
    case State::NoAppToBoot:
    case State::AppUpgradeInProgress:
    case State::ReadyToBoot:
    {
        break;
    }
    }
}

int Bootloader::upgradeApp(IDownloader& downloader)
{
    /*
     * Preparation stage.
     * Note that access to the backend and all members is always protected with the mutex, this is important.
     */
    {
        os::MutexLocker mlock(mutex_);

        switch (state_)
        {
        case State::BootDelay:
        case State::BootCancelled:
        case State::NoAppToBoot:
        {
            state_ = State::AppUpgradeInProgress;
            break;
        }
        case State::ReadyToBoot:
        case State::AppUpgradeInProgress:
        {
            return -ErrInvalidState;
        }
        }

        int res = backend_.beginUpgrade();
        if (res < 0)
        {
            return res;
        }
    }

    DEBUG_LOG("Starting app upgrade...\n");

    /*
     * Downloading stage.
     * New application is downloaded into the storage backend via the Sink proxy class.
     * Every write() is mutex-protected.
     */
    class Sink : public IDownloadStreamSink
    {
        std::size_t offset_ = 0;
        IAppStorageBackend& backend_;
        chibios_rt::Mutex& mutex_;

        int handleNextDataChunk(const void* data, std::size_t size) override
        {
            os::MutexLocker mlock(mutex_);
            int res = backend_.write(offset_, data, size);
            offset_ += size;
            return res;
        }

    public:
        Sink(IAppStorageBackend& back, chibios_rt::Mutex& mutex) :
            backend_(back),
            mutex_(mutex)
        { }
    } sink(backend_, mutex_);

    int res = downloader.download(sink);
    DEBUG_LOG("App download finished with status %d\n", res);

    /*
     * Finalization stage.
     * Checking if the downloader has succeeded, checking if the backend is able to finalize successfully.
     * Notice the mutex.
     */
    os::MutexLocker mlock(mutex_);

    assert(state_ == State::AppUpgradeInProgress);
    state_ = State::NoAppToBoot;                // Default state until proven otherwise

    if (res < 0)                                // Download failed
    {
        (void)backend_.endUpgrade(false);       // Making sure the backend is finalized; error is irrelevant
        return res;
    }

    res = backend_.endUpgrade(true);
    if (res < 0)                                // Finalization failed
    {
        DEBUG_LOG("App storage backend finalization failed (%d)\n", res);
        return res;
    }

    /*
     * Everything went well, checking if the application is valid and updating the state accordingly.
     * This method will report success even if the application image it just downloaded is not valid,
     * since that would be out of the scope of its responsibility.
     */
    verifyAppAndUpdateState();

    return ErrOK;
}

}
