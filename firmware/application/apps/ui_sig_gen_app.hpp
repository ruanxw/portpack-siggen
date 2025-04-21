/*
 * Copyright (C) 2016 Jared Boone, ShareBrained Technology, Inc.
 * Copyright (C) 2016 Furrtek
 * Copyright (C) 2020 Shao
 *
 * This file is part of PortaPack.
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2, or (at your option)
 * any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program; see the file COPYING.  If not, write to
 * the Free Software Foundation, Inc., 51 Franklin Street,
 * Boston, MA 02110-1301, USA.
 */

#ifndef __SIG_GEN_APP_HPP__
#define __SIG_GEN_APP_HPP__

#include "app_settings.hpp"
#include "ui_language.hpp"
#include "radio_state.hpp"
#include "ui_widget.hpp"
#include "ui_navigation.hpp"
#include "ui_receiver.hpp"
#include "ui_freq_field.hpp"
#include "replay_thread.hpp"
#include "ui_spectrum.hpp"
#include "ui_transmitter.hpp"
#include "chvt.h"

#include <string>
#include <memory>

namespace ui {

class SigGenAppView : public View {
   public:
    SigGenAppView(NavigationView& nav);
    ~SigGenAppView();

    void on_hide() override;
    void set_parent_rect(const Rect new_parent_rect) override;
    void focus() override;

    std::string title() const override { return "Signal Gen TX"; };

   private:
    NavigationView& nav_;
    TxRadioState radio_state_{
        1575420000 /* frequency */,
        15000000 /* bandwidth */,
        2600000 /* sampling rate */
    };
    app_settings::SettingsManager settings_{
        "tx_gps", app_settings::Mode::TX};

    static constexpr ui::Dim header_height = 3 * 16;

    const size_t read_size{16384};
    const size_t buffer_count{3};
    std::filesystem::path config_file_name = u"/SigGen/config.txt";

    VirtualTimer cycle_timer;
    bool is_cycle_timer_enabled = false;   //避免在常发状态时，定时器没有启动，但是stop流程会重置定时器导致的中断抢占问题。
    bool is_transmitting = false;     //因为间隔发送时，repaly线程的active状态就不能标志是否发送中，那么更新开始和停止的UI就会有问题，新增flag用来更新UI。

    void on_file_changed(const std::filesystem::path& new_file_path);
    void on_tx_progress(const uint32_t progress);

    void toggle();
    void start();
    void stop(const bool do_loop);
    bool is_active() const;
    void set_ready();
    void handle_replay_thread_done(const uint32_t return_code);
    void file_error();
    void config_file_error();
    void load_last_config();
    void save_last_config();

    void stop_cyclic();
    void start_cycle();
    static void cycle_cb(void* arg);
    void cyclic_tx_ctr(bool CyclicTXCtr);

    std::filesystem::path file_path{};
    std::unique_ptr<ReplayThread> replay_thread{};
    bool ready_signal{false};

    Button button_open{
        {0 * 8, 0 * 16, 10 * 8, 2 * 16},
        "Open file"};

    Button button_load_last_config{
        {0 * 8, 4 * 16, 17 * 8, 2 * 16},
        "load last config"};

    Text text_filename{
        {11 * 8, 0 * 16, 30 * 8, 16},
        "-"};

    Text text_sample_rate{
        {12 * 8, 2 * 16, 6 * 8, 16},
        "-"};

    Text text_duration{
        {11 * 8, 1 * 16, 6 * 8, 16},
        "-"};

    ProgressBar progressbar{
        {18 * 8, 1 * 16, 12 * 8, 16}};

    TxFrequencyField field_frequency{
        {2 * 8, 2 * 16},
        nav_};

    TransmitterView2 tx_view{
        {20 * 8, 2 * 16},
        /*short_ui*/ true};

    Checkbox check_loop{
        {23 * 8, 2 * 16},
        4,
        LanguageHelper::currentMessages[LANG_LOOP],
        true};

    Checkbox check_cycle_enable{
        {0 * 8, 3 * 16},
        15,
        LanguageHelper::currentMessages[LANG_CYCLE_ENABLE],
        true};

    Text text_cycle_tx{
        {17 * 8, 3 * 16, 3 * 8, 1 * 16},
        "T:"};

    NumberField field_cycle_tx{
        {19 * 8, 3 * 16},
        2,
        {1, 30},
        1,
        ' '};

    Text text_cycle_pause{
        {23 * 8, 3 * 16, 3 * 8, 1 * 16},
        "P:"};

    NumberField field_cycle_pause{
        {25 * 8, 3 * 16},
        2,
        {0, 30},
        1,
        ' '};

    ImageButton button_play{
        {0 * 8, 2 * 16, 2 * 8, 1 * 16},
        &bitmap_play,
        Theme::getInstance()->fg_green->foreground,
        Theme::getInstance()->fg_green->background};

    spectrum::WaterfallView waterfall{};

    MessageHandlerRegistration message_handler_replay_thread_error{
        Message::ID::ReplayThreadDone,
        [this](const Message* const p) {
            const auto message = *reinterpret_cast<const ReplayThreadDoneMessage*>(p);
            this->handle_replay_thread_done(message.return_code);
        }};

    MessageHandlerRegistration message_handler_fifo_signal{
        Message::ID::RequestSignal,
        [this](const Message* const p) {
            const auto message = static_cast<const RequestSignalMessage*>(p);
            if (message->signal == RequestSignalMessage::Signal::FillRequest) {
                this->set_ready();
            }
        }};

    MessageHandlerRegistration message_handler_tx_progress{
        Message::ID::TXProgress,
        [this](const Message* const p) {
            const auto message = *reinterpret_cast<const TXProgressMessage*>(p);
            this->on_tx_progress(message.progress);
        }};

    MessageHandlerRegistration message_handler_cyclic_tx_ctr{
        Message::ID::CyclicTxCtr,
        [this](const Message* const p) {
            const auto message = *reinterpret_cast<const CyclicTXCtrMessage*>(p);
            this->cyclic_tx_ctr(message.CyclicTXCtr);
        }};
};

} /* namespace ui */

#endif /*__SIG_GEN_APP_HPP__*/
