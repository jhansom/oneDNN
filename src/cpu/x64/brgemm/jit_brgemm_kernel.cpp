/*******************************************************************************
* Copyright 2020-2024 Intel Corporation
*
* Licensed under the Apache License, Version 2.0 (the "License");
* you may not use this file except in compliance with the License.
* You may obtain a copy of the License at
*
*     http://www.apache.org/licenses/LICENSE-2.0
*
* Unless required by applicable law or agreed to in writing, software
* distributed under the License is distributed on an "AS IS" BASIS,
* WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
* See the License for the specific language governing permissions and
* limitations under the License.
*******************************************************************************/
#include <memory>
#include <vector>

#include "common/c_types_map.hpp"
#include "common/nstl.hpp"
#include "common/type_helpers.hpp"
#include "common/utils.hpp"

#include "cpu/platform.hpp"
#include "cpu/x64/brgemm/brgemm_types.hpp"
#include "cpu/x64/cpu_barrier.hpp"
#include "cpu/x64/injectors/jit_uni_postops_injector.hpp"
#include "cpu/x64/jit_avx512_core_bf16cvt.hpp"
#include "cpu/x64/jit_avx512_core_fp8cvt.hpp"
#include "cpu/x64/jit_generator.hpp"

#define GET_OFF(field) offsetof(brgemm_kernel_params_t, field)
#define GET_OFF_BATCH_ELEMENT(field) offsetof(brgemm_batch_element_t, field)

namespace dnnl {
namespace impl {
namespace cpu {
namespace x64 {

using namespace dnnl::impl::utils;
using namespace Xbyak;
template <typename Wmm>
struct jit_brgemm_kernel_t : public jit_generator {
    jit_brgemm_kernel_t(const brgemm_desc_t &abrg)
        : jit_generator(jit_name(), abrg.isa_impl)
        , brg(abrg)
        , postops_injector_(nullptr)
        , max_effective_vregs(
                  isa_num_vregs(brg.isa_impl) - (brg.is_int8 && !brg.has_int8_vnni ? 2 : (brg.is_fp8_via_convert() ? 5 : 0))
                            - (one_of(brg.dt_b, data_type::nf4) && brg.isa_impl == avx2 ? 5 : 0)
                            - (one_of(brg.dt_b, data_type::nf4) && brg.isa_impl != avx2 ? 1 : 0)
                            - (brg.with_wei_decomp_zero_points && brg.wei_decomp_zero_points_stride == 0 ? 1 : 0)
                            - (brg.with_src_dyn_quant ? 2 : 0)
                            - (brg.with_src_dyn_quant && brg.with_wei_decomp_zero_points && brg.wei_decomp_zero_points_stride != 0 ? brg.ld_block2 : 0)) {

        // The implementation uses is_superset(), is_subset() utilities.
        // So avoid isa_all, isa_undef in these comparisions.
        assert(!utils::one_of(brg.isa_impl, isa_all, isa_undef));
        const int is_ldb2_tail = brg.ldb2_tail ? 1 : 0;
        const int is_ldb_tail = brg.ldb_tail ? 1 : 0;
        is_ldb_loop_ = brg.ldb2 + is_ldb2_tail + is_ldb_tail > 1;

        if (brg.with_eltwise || brg.with_binary || brg.with_sum) {

            static constexpr bool preserve_gpr = true;
            static constexpr bool preserve_vmm = true;
            static constexpr bool use_exact_tail_scalar_bcast = false;
            const auto dst_md_wrapper = memory_desc_wrapper(brg.dst_md());

            static const bcast_set_t enabled_bcast_strategy
                    = {broadcasting_strategy_t::scalar,
                            broadcasting_strategy_t::per_oc,
                            broadcasting_strategy_t::per_oc_spatial,
                            broadcasting_strategy_t::per_mb,
                            broadcasting_strategy_t::per_mb_spatial,
                            broadcasting_strategy_t::per_mb_w,
                            broadcasting_strategy_t::per_w,
                            broadcasting_strategy_t::batch,
                            broadcasting_strategy_t::spatial,
                            broadcasting_strategy_t::no_broadcast};
            const binary_injector::rhs_arg_static_params_t rhs_sp {
                    static_cast<size_t>(vmm_tmp(0).getIdx()), this->r14,
                    this->r15, this->r13, preserve_gpr, preserve_vmm,
                    GET_OFF(post_ops_binary_rhs_arg_vec), GET_OFF(data_C_ptr_),
                    dst_md_wrapper, static_cast<size_t>(brg.ldb_tail),
                    ld_tail_mask, use_exact_tail_scalar_bcast};
            const binary_injector::static_params_t bsp {
                    this->param1, enabled_bcast_strategy, rhs_sp};

            auto st = safe_ptr_assign(postops_injector_,
                    po_injector_t::create(
                            this, brg.isa_impl, brg.attr()->post_ops_, bsp));
            if (st != status::success) {
                assert(!"postops_injector creation failed");
            }

            with_binary_non_scalar_bcast_ = binary_injector::
                    any_binary_postop_rhs_non_scalar_broadcast(
                            brg.attr()->post_ops_, dst_md_wrapper);
        }
        if (brg.is_bf16_emu)
            bf16_emu_ = utils::make_unique<bf16_emulation_t>(this,
                    bf16_emu_reserv_1(), bf16_emu_reserv_2(),
                    bf16_emu_reserv_3(), bf16_emu_scratch, bf16_emu_reserv_4(),
                    bf16_emu_reserv_4());

        if (brg.is_fp8_via_convert()
                && one_of(data_type::f8_e5m2, brg.dt_a, brg.dt_b, brg.dt_c,
                        brg.dt_d))
            // Note: avoid using 'vmm0' since it is used as
            // 'fp8_to_f16_upconvert()' param and would collision with these
            // emulation vmms
            f8_e5m2_emulator_ = utils::make_unique<fp8_emulation_e5m2_t>(this,
                    xmm_fp8_emu_aux2, xmm_fp8_emu_aux3, xmm_fp8_emu_aux4,
                    kmask_fp8_aux, reg64_fp8_aux);
        if (brg.is_fp8_via_convert()
                && one_of(data_type::f8_e4m3, brg.dt_a, brg.dt_b, brg.dt_c,
                        brg.dt_d))
            f8_e4m3_emulator_ = utils::make_unique<fp8_emulation_e4m3_t>(this,
                    xmm_fp8_emu_aux1, xmm_fp8_emu_aux2, xmm_fp8_emu_aux3,
                    xmm_fp8_emu_aux4, xmm_fp8_emu_aux5, reg64_fp8_aux);
    }

    DECLARE_CPU_JIT_AUX_FUNCTIONS(jit_brgemm_kernel_t)

    brgemm_desc_t brg;

private:
    enum matrix_kind_t { matrix_A, matrix_B };
    static constexpr int zmm_width_in_bytes_
            = cpu_isa_traits<avx512_core>::vlen;
    using Vmm =
            typename utils::conditional<std::is_same<Wmm, Xbyak::Tmm>::value,
                    Xbyak::Zmm, Wmm>::type;
    using Vmm_lower_t = typename vreg_traits<Vmm>::Vmm_lower_t;
    using po_injector_t = injector::jit_uni_postops_injector_base_t<Vmm>;
    std::unique_ptr<po_injector_t> postops_injector_;
    std::unique_ptr<bf16_emulation_t> bf16_emu_;
    std::unique_ptr<fp8_emulation_base_t> f8_e5m2_emulator_;
    std::unique_ptr<fp8_emulation_base_t> f8_e4m3_emulator_;

    Xbyak::Label avx_tail_mask_;
    Xbyak::Label sum_zp_scale_data_;
    using reg64_t = const Xbyak::Reg64;

    // Register decomposition
    const reg64_t param1 = abi_param1;

    const reg64_t reg_C = r15;
    const reg64_t reg_aux_C = r14;

    const reg64_t reg_addr_batch = r13;
    const reg64_t reg_A = r13;
    const reg64_t reg_B = r12;

    const reg64_t reg_aux_A = r11;
    const reg64_t reg_aux_B = r10;
    const reg64_t reg_aux_A_vpad = reg_aux_A;

    const reg64_t reg_bdb_loop = r9;
    const reg64_t reg_ldb_loop = r8;

    const reg64_t reg_stride_lda = reg_bdb_loop;
    const reg64_t reg_stride_ldb = reg_ldb_loop;
    const reg64_t reg_stride_ld_block = reg_ldb_loop;
    const reg64_t reg_s8_input_shift = reg_bdb_loop;
    const reg64_t reg_zp_a_input_shift = reg_bdb_loop;

    const reg64_t reg_BS_loop = rax;
    const reg64_t reg_rdb_loop = rbx;
    const reg64_t reg_BS = abi_not_param1;

    const reg64_t reg_a_offset = rdx;
    const reg64_t reg_b_offset = rsi;

    const reg64_t reg_aux1_batch = rbp;
    const reg64_t reg_aux1_A = rbp;
    const reg64_t reg_aux1_B = abi_param1;

    const reg64_t reg_offs_batch = reg_aux1_A;
    const reg64_t reg_strd_batch = reg_rdb_loop;

    const reg64_t reg_bias = reg_rdb_loop;
    const reg64_t reg_scales = reg_rdb_loop;
    const reg64_t reg_aux_bias = reg_rdb_loop;
    const reg64_t reg_dst_scales = reg_rdb_loop;
    const reg64_t reg_zp_comp_a = reg_rdb_loop;
    const reg64_t reg_aux_zp_comp_a = reg_rdb_loop;
    const reg64_t reg_zp_comp_b = reg_rdb_loop;
    const reg64_t reg_aux_zp_comp_b = reg_rdb_loop;
    const reg64_t reg_zp_c_values = reg_rdb_loop;
    const reg64_t reg_aux_zp_c_values = reg_rdb_loop;
    const reg64_t reg_wei_scales = reg_rdb_loop;
    const reg64_t reg_aux_wei_scales = reg_rdb_loop;
    const reg64_t reg_wei_zp = reg_rdb_loop;
    const reg64_t reg_aux_wei_zp = reg_rdb_loop;
    const reg64_t reg_ic = reg_rdb_loop;
    const reg64_t reg_src_scales = reg_rdb_loop;
    const reg64_t reg_tmp_read_values = reg_rdb_loop;

    const reg64_t reg_aux_scales = reg_aux_B;
    const reg64_t reg_aux_dst_scales = reg_aux_B;
    const reg64_t reg_do_post_ops = reg_rdb_loop;
    const reg64_t reg_do_comp = reg_rdb_loop;
    const reg64_t reg_skip_accm = reg_rdb_loop;
    const reg64_t reg_tmp_gpr = reg_rdb_loop;
    const reg64_t reg_ptr_sum_scale = reg_rdb_loop;
    const reg64_t reg_ptr_sum_zp = reg_bdb_loop;
    const reg64_t reg_zp_a_val = reg_rdb_loop;

    const reg64_t reg_buf = reg_rdb_loop;
    const reg64_t reg_buf_aux = abi_param1;
    const reg64_t reg_compensation = reg_rdb_loop;
    const reg64_t reg_aux_compensation = reg_rdb_loop;

    const reg64_t reg_D = reg_aux_A;
    const reg64_t reg_aux_D = reg_BS_loop;

    /* bf16 emulation */
    const reg64_t bf16_emu_scratch = reg_rdb_loop;

    // FP8 Convert
    const reg64_t reg_converted_stride = reg_rdb_loop;
    const reg64_t reg64_fp8_aux = reg_A;

    constexpr static int origin_offs_batch_offs_ = 0;
    constexpr static int origin_strd_batch_offs_ = 0;
    constexpr static int reg_bias_offs_ = 8;
    constexpr static int reg_aux_bias_offs_ = 16;
    constexpr static int reg_do_post_ops_offs_ = 24;
    constexpr static int reg_D_offs_ = 32;
    constexpr static int reg_aux_D_offs_ = 40;
    constexpr static int reg_scales_offs_ = 48;
    constexpr static int reg_aux_scales_offs_ = 56;
    constexpr static int reg_bdb_loop_offs_ = 64;
    constexpr static int reg_ldb_loop_offs_ = 72;
    constexpr static int reg_buf_offs_ = 80;
    constexpr static int reg_comp_offs_ = reg_buf_offs_;
    constexpr static int reg_aux_comp_offs_ = 88;
    constexpr static int abi_param1_offs_ = 96;
    constexpr static int reg_zp_comp_a_offs_ = 104;
    constexpr static int reg_aux_zp_comp_a_offs_ = 112;
    constexpr static int reg_zp_comp_b_offs_ = 120;
    constexpr static int reg_aux_zp_comp_b_offs_ = 128;
    constexpr static int reg_zp_c_values_offs_ = 136;
    constexpr static int reg_aux_zp_c_values_offs_ = 144;
    constexpr static int reg_data_C_ptr_ = 152;
    constexpr static int reg_skip_accm_offs_ = 160;
    constexpr static int reg_zp_a_val_offs_ = 168;
    constexpr static int reg_do_comp_offs_ = 176;
    constexpr static int reg_dst_scales_offs_ = 184;
    constexpr static int reg_C_shift_bytes_offs_ = 192;
    constexpr static int reg_aux_C_backup_offs_ = 200;
    constexpr static int reg_aux_C_bdb_loop_backup_offs_ = 208;
    constexpr static int reg_aux_C_bdb_loop_shift_offs_ = 216;
    constexpr static int reg_D_shift_bytes_offs_ = 224;
    constexpr static int reg_aux_D_backup_offs_ = 232;
    constexpr static int reg_aux_D_bdb_loop_backup_offs_ = 240;
    constexpr static int reg_aux_D_bdb_loop_shift_offs_ = 248;
    constexpr static int reg_wei_scales_offs_ = 256;
    constexpr static int reg_aux_wei_scales_offs_ = 264;
    constexpr static int reg_wei_zero_points_offs_ = 272;
    constexpr static int reg_aux_wei_zero_points_offs_ = 280;
    constexpr static int reg_ic_offs_ = 288;
    constexpr static int reg_aux2_D_offs_ = 296;
    constexpr static int reg_aux2_wei_scales_offs_ = 304;
    constexpr static int reg_aux2_wei_zero_points_offs_ = 312;
    constexpr static int reg_aux_ic_offs_ = 320;
    constexpr static int reg_reg_a_offset_offs_ = 328;
    constexpr static int reg_src_scales_offs_ = 336;
    constexpr static int reg_aux_src_scales_offs_ = 344;
    constexpr static int reg_aux2_src_scales_offs_ = 352;
    // constexpr static int stack_space_needed_ = 360;
    // these are used for FP8 as temporary push/pop spaces
    constexpr static int reg_val_tmp_1_ = 368;
    constexpr static int reg_val_tmp_2_ = 376;
    constexpr static int stack_space_needed_ = 384;
    // regsiters for dynamic quant


    bool is_ldb_loop_ = false;
    bool with_binary_non_scalar_bcast_ = false;
    const int max_effective_vregs;

    Xbyak::Opmask ld_full_mask = Xbyak::Opmask(2);
    Xbyak::Opmask ld_tail_mask = Xbyak::Opmask(3);
    Xbyak::Opmask fp8_col_mask = Xbyak::Opmask(4);
    Xbyak::Opmask kmask_fp8_aux = Xbyak::Opmask(5);

    Vmm accm(int ld_block, int bd, int ld) {
        return Vmm(max_effective_vregs - 1 - (bd * ld_block + ld));
    }

    Vmm bcst(int bd = 0) {
        if (n_bcast_1_load) {
            int idx = max_effective_vregs - 1 - (brg.ld_block2 * brg.bd_block)
                    - bd;
            assert(idx > 0);
            return Vmm(idx);
        } else
            return Vmm(0);
    }

    Vmm load(int ld = 0) {
        if (n_bcast_1_load) {
            return Vmm(0);
        } else {
            int idx = max_effective_vregs - 1 - (brg.ld_block2 * brg.bd_block)
                    - ld;
            assert(idx > 0);
            return Vmm(idx);
        }
    }

    Vmm vmm_tmp(int i) {
        assert(IMPLICATION(!brg.is_tmm,
                i >= 0
                        && i < max_effective_vregs
                                        - brg.bd_block * brg.ld_block2));
        return Vmm(i);
    }

    Vmm vmm_tail_mask() { return vmm_tmp(1); }
    Vmm vmm_one_bytes() const noexcept { return Vmm(3); }
    Vmm vmm_zp_a_shift() const noexcept { return Vmm(2); }
    Vmm vmm_inp_shift() const noexcept { return Vmm(1); }

    /* bf16 emulation */
    Zmm bf16_emu_reserv_1() const noexcept { return Zmm(0); }
    Zmm bf16_emu_reserv_2() const noexcept { return Zmm(1); }
    Zmm bf16_emu_reserv_3() const noexcept { return Zmm(2); }
    Zmm bf16_emu_reserv_4() const noexcept { return Zmm(3); }
    // note: zmm reserv_5 is not necessary since it's only used for 'vdpbf16ps'

    // fp8 emulation convert
    Vmm xmm_fp8_emu_aux1 = Vmm(0);
    Vmm xmm_fp8_emu_aux2 = Vmm(1);
    Vmm xmm_fp8_emu_aux3 = Vmm(2);
    Vmm xmm_fp8_emu_aux4 = Vmm(3);
    Vmm xmm_fp8_emu_aux5 = Vmm(4);

    // Required in every dot product for INT8 non-VNNI computation.
    Vmm int8_ones_words() const noexcept {
        return Vmm(isa_num_vregs(brg.isa_impl) - 1);
    }
    Vmm int8_dot_product_temp() const noexcept {
        return Vmm(isa_num_vregs(brg.isa_impl) - 2);
    }

    Vmm vmm_mask(const Vmm vmm_in, bool mask_flag, bool store,
            Xbyak::Opmask ktail_mask) const;
    Vmm_lower_t vmm_lower_mask(const Vmm_lower_t vmm_lower_in, bool mask_flag,
            bool store, Xbyak::Opmask ktail_mask) const;
    void maybe_set_avx_mask(bool is_ld_tail);

    void cvt2ps(data_type_t type_in, const Vmm vmm_in, const Xbyak::Operand &op,
            bool mask_flag, bool store, Xbyak::Opmask ktail_mask,
            int tail_size);

    void advance_ldb_post_op_regs();
    void restore_ldb_post_op_regs(int ld_block2);
    void advance_bdb_post_op_regs(int adj_bd_block);
    void restore_bdb_post_op_regs(int bd_block2);
    void ldb_regs_shift(int ld_block2, bool is_tail = false);
    void advance_bd_block2_post_op_regs(int bd_block2);

    void copy_post_ops_stack_values_to_aux(bool is_reg_tail);
    void read_params();
    void zero_accumulators(int bd_block2, bool is_bdb_tail, int ld_block,
            bool is_ld_tail, bool skip_accumulation);

    void fp8_to_f16_upconvert(int num_rows, int tile_num_col_bytes,
            reg64_t reg_base, int offset, reg64_t reg_data_stride,
            data_type_t dt, bool is_rd_tail);
    void fp8_to_f16_upconvert_to_vnni(int num_rows, int tile_num_col_bytes,
            reg64_t reg_base, int offset, reg64_t reg_data_stride,
            data_type_t dt, bool is_rd_tail);
    void store_accumulators(int bd_block2, bool is_bdb_tail, int ld_block,
            bool is_ld_tail, bool skip_accumulation);
    void store_accumulators_without_post_ops(
            int bd_block, int ld_block, bool is_ld_tail);
    void store_accumulators_apply_post_ops(int bd_block, int ld_block,
            int ldb_and_bdb_offset, bool is_ld_tail);
    void apply_compensation(int bd_block, int ld_block, bool is_ld_tail);
    void apply_alpha_beta(int bd_block, int ld_block, bool is_ld_tail);
    void apply_post_ops(int bd_block, int ld_block2, int ldb_and_bdb_offset,
            bool is_ld_tail);
    void restore_A_B_matrices();
    void set_A_B_matrices();

    void compute_int8_compensation(int rd_loop, int bd_b, int bd_e,
            int bd_block, int ld_block2, bool is_ld_tail, int vpad);
    void maybe_pre_process_data(matrix_kind_t matrix_kind, const Tmm &t1,
            reg64_t reg_base, size_t offset, reg64_t reg_stride, int num_rows,
            int num_col_bytes, bool is_rd_tail);
    void maybe_tileloadd_nt(matrix_kind_t matrix_kind, int idx, int offset,
            bool is_rd_tail, bool is_tail);
    void dot_product(Vmm v1, Vmm v2, Vmm v3);
    void gemm_microkernel(int bd_block2, bool is_bdb_tail, int ld_block,
            bool is_rd_tail, bool is_ld_tail, int vpad, int rows_for_rd_tail);
    void gemm_microkernel_amx(int bd_block2, bool is_bdb_tail, int ld_block,
            bool is_rd_tail, bool is_ld_tail);
    void gemm_microkernel_dyn_quant(int bd_block2, bool is_bdb_tail, int ld_block,
            bool is_rd_tail, bool is_ld_tail, int vpad, int rows_for_rd_tail);

    void ldb_loop(int bd_block2, bool is_bdb_tail, int ld_block,
            int ldb_loop_length, bool is_reg_tail, bool is_ld_tail,
            bool check_top_vpad, bool check_bottom_vpad, int rows_for_rd_tail,
            bool skip_accumulation);
    void bdb_loop();

    void generate() override;

    int A_offset(int bd, int rd, bool is_amx = false) const noexcept;
    int B_offset(int ld, int rd, bool is_amx = false) const noexcept;
    int C_offset(int bd, int ld) const noexcept;
    int D_offset(int bd, int ld) const noexcept;

    int rdb_A_offset() const noexcept;
    int rdb_B_offset() const noexcept;

    int ldb_B_offset(int ld_block2, bool is_tail = false) const noexcept;
    int ldb_C_offset(int ld_block2, bool is_tail = false) const noexcept;
    int ldb_D_offset(int ld_block2, bool is_tail = false) const noexcept;
    int ldb_po_offset(int ld_block2, bool is_tail = false) const noexcept;

    int bdb_A_offset(int bd_block2) const noexcept;
    int bdb_C_offset(int bd_block2) const noexcept;
    int bdb_D_offset(int bd_block2) const noexcept;
    int bdb_po_offset(int bd_block2) const noexcept;

    int bias_offset(int ld, bool is_tail = false) const noexcept;
    int oc_logical_offset(int ld, bool is_tail = false) const noexcept;

    int compensations_offset(int ld, bool is_tail = false) const noexcept;
    int bdb_compensation_offset(int bd_block2) const noexcept;
    int bd_compensation_offset(int ld, int bd) const noexcept;
    int scales_offset(int ld, bool is_tail = false) const noexcept;
    int wei_scales_offset(int ld, bool is_tail = false) const noexcept;
    int wei_zp_offset(int ld, bool is_tail = false) const noexcept;
    int zp_comp_a_offset(int ld, bool is_tail = false) const noexcept;
    int bd_zp_comp_a_offset(int ld, int bd) const noexcept;
    int bdb_zp_comp_a_offset(int bd_block2) const noexcept;
    int zp_comp_b_offset(int bd) const noexcept;
    int bdb_zp_comp_b_offset(int bd_block2) const noexcept;
    int zp_c_values_offset(int ld, bool is_tail = false) const noexcept;

    bool n_bcast_1_load = false;
    bool vpad_exist = false;
    bool need_comp_pads = false;
};

template <typename Wmm>
int jit_brgemm_kernel_t<Wmm>::A_offset(
        int bd, int rd, bool is_amx) const noexcept {
    return (is_amx) ? brg.typesize_A * (bd * brg.bd_block * brg.LDA)
                    : brg.typesize_A * (bd * brg.LDA + rd);
}

template <typename Wmm>
int jit_brgemm_kernel_t<Wmm>::B_offset(
        int ld, int rd, bool is_amx) const noexcept {
    int typesize_scale = one_of(brg.dt_b, data_type::nf4, data_type::s4, data_type::u4) ? 2 : 1;
    if (is_amx) {
        return brg.typesize_B * (brg.rd_step * ld * brg.ld_block) / typesize_scale;
    } else {
        const int data_vnni_granularity = brg.ld_step;
        const int rdb0 = rd / data_vnni_granularity;
        // Note: Offsets for elements within vnni_granularity are expected to be
        // handled within gemm_microkernel (for ex: odd-even converts).
        // hence no `rd % data_vnni_granularity`
        return brg.typesize_B
                * (rdb0 * data_vnni_granularity * brg.LDB
                        + data_vnni_granularity * ld * brg.ld_block) / typesize_scale;
    }
}

template <typename Wmm>
int jit_brgemm_kernel_t<Wmm>::C_offset(int bd, int ld) const noexcept {
    const auto bd_shift = brg.is_runtime_ldc ? 0 : bd * brg.LDC;
    return brg.typesize_C * (bd_shift + ld * brg.ld_block);
}

template <typename Wmm>
int jit_brgemm_kernel_t<Wmm>::D_offset(int bd, int ld) const noexcept {
    const auto bd_shift = brg.is_runtime_ldd ? 0 : bd * brg.LDD;
    return brg.typesize_D * (bd_shift + ld * brg.ld_block);
}

template <typename Wmm>
int jit_brgemm_kernel_t<Wmm>::rdb_A_offset() const noexcept {
    return brg.typesize_A * brg.rd_block;
}

template <typename Wmm>
int jit_brgemm_kernel_t<Wmm>::rdb_B_offset() const noexcept {
    int typesize_scale = one_of(brg.dt_b, data_type::nf4, data_type::s4, data_type::u4) ? 2 : 1;
    return brg.typesize_B * brg.rd_block * brg.LDB / typesize_scale;
}

template <typename Wmm>
int jit_brgemm_kernel_t<Wmm>::ldb_B_offset(
        int ld_block2, bool is_tail) const noexcept {
    int typesize_scale = one_of(brg.dt_b, data_type::nf4, data_type::s4, data_type::u4) ? 2 : 1;
    return (is_tail) ? brg.typesize_B * brg.ldb_tail * brg.ld_step / typesize_scale
                     : brg.typesize_B * ld_block2 * brg.ld_block * brg.ld_step / typesize_scale;
}

template <typename Wmm>
int jit_brgemm_kernel_t<Wmm>::ldb_C_offset(
        int ld_block2, bool is_tail) const noexcept {
    return (is_tail) ? brg.typesize_C * brg.ldb_tail
                     : brg.typesize_C * ld_block2 * brg.ld_block;
}

template <typename Wmm>
int jit_brgemm_kernel_t<Wmm>::ldb_D_offset(
        int ld_block2, bool is_tail) const noexcept {
    return (is_tail) ? brg.typesize_D * brg.ldb_tail
                     : brg.typesize_D * ld_block2 * brg.ld_block;
}

template <typename Wmm>
int jit_brgemm_kernel_t<Wmm>::ldb_po_offset(
        int ld_block2, bool is_tail) const noexcept {
    return (is_tail) ? brg.ldb_tail : ld_block2 * brg.ld_block;
}

template <typename Wmm>
int jit_brgemm_kernel_t<Wmm>::bdb_A_offset(int bd_block2) const noexcept {
    return brg.typesize_A * bd_block2 * brg.bd_block * brg.LDA;
}

template <typename Wmm>
int jit_brgemm_kernel_t<Wmm>::bdb_C_offset(int bd_block2) const noexcept {
    return bd_block2 * brg.bd_block
            * (brg.is_runtime_ldc ? 1 : brg.typesize_C * brg.LDC);
}

template <typename Wmm>
int jit_brgemm_kernel_t<Wmm>::bdb_D_offset(int bd_block2) const noexcept {
    return bd_block2 * brg.bd_block
            * (brg.is_runtime_ldd ? 1 : brg.typesize_D * brg.LDD);
}

template <typename Wmm>
int jit_brgemm_kernel_t<Wmm>::bdb_po_offset(int bd_block2) const noexcept {
    return bd_block2 * brg.bd_block * brg.LDD;
}

template <typename Wmm>
int jit_brgemm_kernel_t<Wmm>::bias_offset(int ld, bool is_tail) const noexcept {
    return (is_tail) ? brg.typesize_bias * brg.ldb_tail
                     : brg.typesize_bias * ld * brg.ld_block;
}

template <typename Wmm>
int jit_brgemm_kernel_t<Wmm>::oc_logical_offset(
        int ld, bool is_tail) const noexcept {
    return (is_tail) ? brg.ldb_tail : ld * brg.ld_block;
}

template <typename Wmm>
int jit_brgemm_kernel_t<Wmm>::compensations_offset(
        int ld, bool is_tail) const noexcept {
    return (is_tail) ? sizeof(int32_t) * brg.ldb_tail
                     : sizeof(int32_t) * ld * brg.ld_block;
}

template <typename Wmm>
int jit_brgemm_kernel_t<Wmm>::bdb_compensation_offset(
        int bd_block2) const noexcept {
    return sizeof(int32_t) * bd_block2 * brg.bd_block * brg.LDB;
}

template <typename Wmm>
int jit_brgemm_kernel_t<Wmm>::bd_compensation_offset(
        int ld, int bd) const noexcept {
    return sizeof(int32_t) * (ld * brg.ld_block + bd * brg.LDB);
}

template <typename Wmm>
int jit_brgemm_kernel_t<Wmm>::scales_offset(
        int ld, bool is_tail) const noexcept {
    return (is_tail) ? brg.is_oc_scale * sizeof(float) * brg.ldb_tail
                     : brg.is_oc_scale * sizeof(float) * ld * brg.ld_block;
}

template <typename Wmm>
int jit_brgemm_kernel_t<Wmm>::wei_scales_offset(
        int ld, bool is_tail) const noexcept {
    return (is_tail) ? sizeof(float) * brg.ldb_tail
                     : sizeof(float) * ld * brg.ld_block;
}

template <typename Wmm>
int jit_brgemm_kernel_t<Wmm>::wei_zp_offset(
        int ld, bool is_tail) const noexcept {
    return (is_tail) ? types::data_type_size(brg.wei_decomp_zero_points_dt) * brg.ldb_tail
                     : types::data_type_size(brg.wei_decomp_zero_points_dt) * ld * brg.ld_block;
}

template <typename Wmm>
int jit_brgemm_kernel_t<Wmm>::zp_comp_a_offset(
        int ld, bool is_tail) const noexcept {
    return (is_tail) ? sizeof(int32_t) * brg.ldb_tail
                     : sizeof(int32_t) * ld * brg.ld_block;
}

template <typename Wmm>
int jit_brgemm_kernel_t<Wmm>::bdb_zp_comp_a_offset(
        int bd_block2) const noexcept {
    return sizeof(int32_t) * bd_block2 * brg.bd_block * brg.LDB;
}

template <typename Wmm>
int jit_brgemm_kernel_t<Wmm>::bd_zp_comp_a_offset(
        int ld, int bd) const noexcept {
    return sizeof(int32_t) * (ld * brg.ld_block + bd * brg.LDB);
}

template <typename Wmm>
int jit_brgemm_kernel_t<Wmm>::zp_comp_b_offset(int bd) const noexcept {
    return sizeof(int32_t) * bd;
}

template <typename Wmm>
int jit_brgemm_kernel_t<Wmm>::bdb_zp_comp_b_offset(
        int bd_block2) const noexcept {
    return zp_comp_b_offset(bd_block2 * brg.bd_block);
}

template <typename Wmm>
int jit_brgemm_kernel_t<Wmm>::zp_c_values_offset(
        int ld, bool is_tail) const noexcept {
    if (brg.zp_type_c == brgemm_broadcast_t::per_n) {
        return (is_tail) ? sizeof(int32_t) * brg.ldb_tail
                         : sizeof(int32_t) * ld * brg.ld_block;
    }

    return 0;
}
template <typename Wmm>
typename jit_brgemm_kernel_t<Wmm>::Vmm jit_brgemm_kernel_t<Wmm>::vmm_mask(
        const Vmm vmm_in, bool mask_flag, bool store,
        Xbyak::Opmask ktail_mask) const {
    return mask_flag && is_superset(brg.isa_impl, avx512_core)
            ? (store ? vmm_in | ktail_mask : vmm_in | ktail_mask | T_z)
            : vmm_in;
}

template <typename Wmm>
typename jit_brgemm_kernel_t<Wmm>::Vmm_lower_t
jit_brgemm_kernel_t<Wmm>::vmm_lower_mask(const Vmm_lower_t vmm_lower_in,
        bool mask_flag, bool store, Xbyak::Opmask ktail_mask) const {
    return mask_flag && is_superset(brg.isa_impl, avx512_core)
            ? (store ? vmm_lower_in | ktail_mask
                     : vmm_lower_in | ktail_mask | T_z)
            : vmm_lower_in;
}

template <typename Wmm>
void jit_brgemm_kernel_t<Wmm>::maybe_set_avx_mask(bool is_ld_tail) {
    if (IMPLICATION(is_ld_tail, isa_has_masks(brg.isa_impl))) return;
    vmovups(vmm_tail_mask(), ptr[rip + avx_tail_mask_]);
}

template <typename Wmm>
void jit_brgemm_kernel_t<Wmm>::cvt2ps(data_type_t type_in, const Vmm vmm_in,
        const Xbyak::Operand &op, bool mask_flag, bool store,
        Xbyak::Opmask ktail_mask, int tail_size) {
    Vmm vmm = vmm_in;
    const bool has_tail
            = op.isMEM() && tail_size != vreg_traits<Vmm>::vlen / sizeof(float);
    if (IMPLICATION(has_tail, is_superset(brg.isa_impl, avx512_core))) {
        vmm = vmm_mask(vmm_in, mask_flag, store, ktail_mask);
    } else {
        load_data(type_in, vmm_in, op.getAddress(), tail_size);
        if (types::is_integral_dt(type_in)) uni_vcvtdq2ps(vmm_in, vmm_in);
        return;
    }
    switch (type_in) {
        case data_type::f32:
        case data_type::s32: uni_vmovups(vmm, op); break;
        case data_type::bf16:
            uni_vpmovzxwd(vmm, op);
            uni_vpslld(vmm, vmm, 16);
            break;
        case data_type::f16: vcvtph2ps(vmm, op); break;
        case data_type::s8: uni_vpmovsxbd(vmm, op); break;
        case data_type::u8: uni_vpmovzxbd(vmm, op); break;
        case data_type::f8_e5m2:
            if (brg.is_fp8_via_convert()) {
                // note: unoptimized, probably move stack use outside loop
                mov(ptr[rsp + reg_val_tmp_1_], reg64_fp8_aux);
                f8_e5m2_emulator_->vcvt_f8_to_f32(vmm, op);
                mov(reg64_fp8_aux, ptr[rsp + reg_val_tmp_1_]);
            } else
                assert(!"Error, native conversion unsupported");
            break;
        case data_type::f8_e4m3:
            if (brg.is_fp8_via_convert()) {
                // note: unoptimized, probably move stack use outside loop
                mov(ptr[rsp + reg_val_tmp_1_], reg64_fp8_aux);
                f8_e4m3_emulator_->vcvt_f8_to_f32(vmm, op);
                mov(reg64_fp8_aux, ptr[rsp + reg_val_tmp_1_]);
            } else
                assert(!"Error, native conversion unsupported");
            break;

        default: assert(!"unsupported data type");
    }
    if (types::is_integral_dt(type_in)) uni_vcvtdq2ps(vmm_in, vmm_in);
}

template <typename Wmm>
void jit_brgemm_kernel_t<Wmm>::advance_ldb_post_op_regs() {
    if (brg.with_bias) {
        mov(reg_aux_bias, ptr[rsp + reg_aux_bias_offs_]);
        add(reg_aux_bias, bias_offset(1));
        mov(ptr[rsp + reg_aux_bias_offs_], reg_aux_bias);
    }
    if (brg.with_scales) {
        mov(reg_aux_scales, ptr[rsp + reg_aux_scales_offs_]);
        add(reg_aux_scales, scales_offset(1));
        mov(ptr[rsp + reg_aux_scales_offs_], reg_aux_scales);
    }
    if (brg.zp_type_a != brgemm_broadcast_t::none) {
        mov(reg_aux_zp_comp_a, ptr[rsp + reg_aux_zp_comp_a_offs_]);
        add(reg_aux_zp_comp_a, zp_comp_a_offset(1));
        mov(ptr[rsp + reg_aux_zp_comp_a_offs_], reg_aux_zp_comp_a);
    }
    if (brg.zp_type_c == brgemm_broadcast_t::per_n) {
        mov(reg_aux_zp_c_values, ptr[rsp + reg_aux_zp_c_values_offs_]);
        add(reg_aux_zp_c_values, zp_c_values_offset(1));
        mov(ptr[rsp + reg_aux_zp_c_values_offs_], reg_aux_zp_c_values);
    }
}

template <typename Wmm>
void jit_brgemm_kernel_t<Wmm>::restore_ldb_post_op_regs(int ld_block2) {
    if (brg.with_bias) {
        mov(reg_aux_bias, ptr[rsp + reg_aux_bias_offs_]);
        sub(reg_aux_bias, bias_offset(ld_block2 - 1));
        mov(ptr[rsp + reg_aux_bias_offs_], reg_aux_bias);
    }
    if (brg.with_scales) {
        mov(reg_aux_scales, ptr[rsp + reg_aux_scales_offs_]);
        sub(reg_aux_scales, scales_offset(ld_block2 - 1));
        mov(ptr[rsp + reg_aux_scales_offs_], reg_aux_scales);
    }
    if (brg.zp_type_a != brgemm_broadcast_t::none) {
        mov(reg_aux_zp_comp_a, ptr[rsp + reg_aux_zp_comp_a_offs_]);
        sub(reg_aux_zp_comp_a, zp_comp_a_offset(ld_block2 - 1));
        mov(ptr[rsp + reg_aux_zp_comp_a_offs_], reg_aux_zp_comp_a);
    }
    if (brg.zp_type_c == brgemm_broadcast_t::per_n) {
        mov(reg_aux_zp_c_values, ptr[rsp + reg_aux_zp_c_values_offs_]);
        sub(reg_aux_zp_c_values, zp_c_values_offset(ld_block2 - 1));
        mov(ptr[rsp + reg_aux_zp_c_values_offs_], reg_aux_zp_c_values);
    }
}

template <typename Wmm>
void jit_brgemm_kernel_t<Wmm>::advance_bdb_post_op_regs(int adj_bd_block) {
    if (brg.zp_type_b != brgemm_broadcast_t::none) {
        mov(reg_aux_zp_comp_b, ptr[rsp + reg_aux_zp_comp_b_offs_]);
        add(reg_aux_zp_comp_b, bdb_zp_comp_b_offset(1));
        mov(ptr[rsp + reg_aux_zp_comp_b_offs_], reg_aux_zp_comp_b);
    }
    if (brg.req_comp_pads_with_bcast
            && brg.zp_type_a != brgemm_broadcast_t::none) {
        mov(reg_aux_zp_comp_a, ptr[rsp + reg_aux_zp_comp_a_offs_]);
        add(reg_aux_zp_comp_a, bdb_compensation_offset(1));
        mov(ptr[rsp + reg_aux_zp_comp_a_offs_], reg_aux_zp_comp_a);
    }
}

template <typename Wmm>
void jit_brgemm_kernel_t<Wmm>::restore_bdb_post_op_regs(int bd_block2) {
    bool post_processed = false;
    if (bd_block2 > 1) {
        if (brg.zp_type_b != brgemm_broadcast_t::none) {
            post_processed = true;
            mov(reg_aux_zp_comp_b, ptr[rsp + reg_aux_zp_comp_b_offs_]);
            sub(reg_aux_zp_comp_b, bdb_zp_comp_b_offset(bd_block2 - 1));
            mov(ptr[rsp + reg_aux_zp_comp_b_offs_], reg_aux_zp_comp_b);
        }
        if (brg.req_comp_pads_with_bcast
                && brg.zp_type_a != brgemm_broadcast_t::none) {
            mov(reg_aux_zp_comp_a, ptr[rsp + reg_aux_zp_comp_a_offs_]);
            sub(reg_aux_zp_comp_a, bdb_compensation_offset(bd_block2 - 1));
            mov(ptr[rsp + reg_aux_zp_comp_a_offs_], reg_aux_zp_comp_a);
        }
    }
    if (post_processed) mov(reg_buf, ptr[rsp + reg_buf_offs_]);
}

template <typename Wmm>
void jit_brgemm_kernel_t<Wmm>::ldb_regs_shift(int ld_block2, bool is_tail) {
    int C_offset = (is_tail) ? ldb_C_offset(1, true) : ldb_C_offset(ld_block2);
    int D_offset = (is_tail) ? ldb_D_offset(1, true) : ldb_D_offset(ld_block2);
    add(reg_aux_C, C_offset);
    add(reg_aux_D, D_offset);

    add(reg_b_offset,
            (is_tail) ? ldb_B_offset(1, true) : ldb_B_offset(ld_block2));

    if (brg.with_bias) {
        mov(reg_aux_bias, ptr[rsp + reg_aux_bias_offs_]);
        add(reg_aux_bias,
                (is_tail) ? bias_offset(1, true) : bias_offset(ld_block2));
        mov(ptr[rsp + reg_aux_bias_offs_], reg_aux_bias);
    }
    if (brg.req_s8s8_compensation) {
        mov(reg_aux_compensation, ptr[rsp + reg_aux_comp_offs_]);
        add(reg_aux_compensation,
                (is_tail) ? compensations_offset(1, true)
                          : compensations_offset(ld_block2));
        mov(ptr[rsp + reg_aux_comp_offs_], reg_aux_compensation);
    }
    if (brg.with_scales) {
        mov(reg_aux_scales, ptr[rsp + reg_aux_scales_offs_]);
        add(reg_aux_scales,
                (is_tail) ? scales_offset(1, true) : scales_offset(ld_block2));
        mov(ptr[rsp + reg_aux_scales_offs_], reg_aux_scales);
    }

    if (brg.with_wei_decomp) {
        mov(reg_aux_wei_scales, ptr[rsp + reg_aux_wei_scales_offs_]);
        add(reg_aux_wei_scales, (is_tail) ? wei_scales_offset(1, true) : wei_scales_offset(ld_block2));
        mov(ptr[rsp + reg_aux_wei_scales_offs_], reg_aux_wei_scales);
        mov(ptr[rsp + reg_aux2_wei_scales_offs_], reg_aux_wei_scales);

        mov(reg_aux_wei_zp, ptr[rsp + reg_aux_wei_zero_points_offs_]);
        add(reg_aux_wei_zp, (is_tail) ? wei_zp_offset(1, true) : wei_zp_offset(ld_block2));
        mov(ptr[rsp + reg_aux_wei_zero_points_offs_], reg_aux_wei_zp);
        mov(ptr[rsp + reg_aux2_wei_zero_points_offs_], reg_aux_wei_zp);
    }

    if (brg.zp_type_a != brgemm_broadcast_t::none) {
        mov(reg_aux_zp_comp_a, ptr[rsp + reg_aux_zp_comp_a_offs_]);
        add(reg_aux_zp_comp_a,
                (is_tail) ? zp_comp_a_offset(1, true)
                          : zp_comp_a_offset(ld_block2));
        mov(ptr[rsp + reg_aux_zp_comp_a_offs_], reg_aux_zp_comp_a);
    }
    if (brg.zp_type_c == brgemm_broadcast_t::per_n) {
        mov(reg_aux_zp_c_values, ptr[rsp + reg_aux_zp_c_values_offs_]);
        add(reg_aux_zp_c_values,
                (is_tail) ? zp_c_values_offset(1, true)
                          : zp_c_values_offset(ld_block2));
        mov(ptr[rsp + reg_aux_zp_c_values_offs_], reg_aux_zp_c_values);
    }
}

template <typename Wmm>
void jit_brgemm_kernel_t<Wmm>::advance_bd_block2_post_op_regs(int bd_block2) {
    if (brg.req_comp_pads_with_bcast && brg.req_s8s8_compensation) {
        mov(reg_compensation, ptr[rsp + reg_comp_offs_]);
        add(reg_compensation, bdb_compensation_offset(bd_block2));
        mov(ptr[rsp + reg_comp_offs_], reg_compensation);
    }

    if (brg.req_comp_pads_with_bcast
            && brg.zp_type_a != brgemm_broadcast_t::none) {
        mov(reg_zp_comp_a, ptr[rsp + reg_zp_comp_a_offs_]);
        add(reg_zp_comp_a, bdb_zp_comp_a_offset(bd_block2));
        mov(ptr[rsp + reg_zp_comp_a_offs_], reg_zp_comp_a);
    }

    if (brg.zp_type_b != brgemm_broadcast_t::none) {
        mov(reg_zp_comp_b, ptr[rsp + reg_zp_comp_b_offs_]);
        add(reg_zp_comp_b, bdb_zp_comp_b_offset(bd_block2));
        mov(ptr[rsp + reg_zp_comp_b_offs_], reg_zp_comp_b);
    }
}

template <typename Wmm>
void jit_brgemm_kernel_t<Wmm>::copy_post_ops_stack_values_to_aux(
        bool is_reg_tail) {
    if (!is_reg_tail) {
        mov(reg_aux_C, reg_C);
        mov(reg_aux_D, reg_D);
        xor_(reg_b_offset, reg_b_offset);
        if (brg.with_bias) {
            mov(reg_bias, ptr[rsp + reg_bias_offs_]);
            mov(ptr[rsp + reg_aux_bias_offs_], reg_bias);
        }
        if (brg.req_s8s8_compensation) {
            mov(reg_compensation, ptr[rsp + reg_comp_offs_]);
            mov(ptr[rsp + reg_aux_comp_offs_], reg_compensation);
        }
        if (brg.with_scales) {
            mov(reg_scales, ptr[rsp + reg_scales_offs_]);
            mov(ptr[rsp + reg_aux_scales_offs_], reg_scales);
        }

        if (brg.zp_type_a != brgemm_broadcast_t::none) {
            mov(reg_zp_comp_a, ptr[rsp + reg_zp_comp_a_offs_]);
            mov(ptr[rsp + reg_aux_zp_comp_a_offs_], reg_zp_comp_a);
        }

        if (brg.zp_type_c != brgemm_broadcast_t::none) {
            mov(reg_zp_c_values, ptr[rsp + reg_zp_c_values_offs_]);
            mov(ptr[rsp + reg_aux_zp_c_values_offs_], reg_zp_c_values);
        }

        if (brg.with_wei_decomp_scales) {
            mov(reg_wei_scales, ptr[rsp + reg_wei_scales_offs_]);
            mov(ptr[rsp + reg_aux_wei_scales_offs_], reg_wei_scales);
            mov(ptr[rsp + reg_aux2_wei_scales_offs_], reg_wei_scales);
        }
        if (brg.with_wei_decomp_zero_points) {
            mov(reg_wei_zp, ptr[rsp + reg_wei_zero_points_offs_]);
            mov(ptr[rsp + reg_aux_wei_zero_points_offs_], reg_wei_zp);
            mov(ptr[rsp + reg_aux2_wei_zero_points_offs_], reg_wei_zp);
        }

    }
    if (brg.with_grouped_wei_decomp) {
        mov(reg_ic, ptr[rsp + reg_ic_offs_]);
        mov(ptr[rsp + reg_aux_ic_offs_], reg_ic);
    }
    if (brg.with_src_dyn_quant) {
        mov(reg_src_scales, ptr[rsp + reg_src_scales_offs_]);
        mov(ptr[rsp + reg_aux_src_scales_offs_], reg_src_scales);
        mov(ptr[rsp + reg_aux2_src_scales_offs_], reg_src_scales);
    }
    if (brg.zp_type_b != brgemm_broadcast_t::none) {
        mov(reg_zp_comp_b, ptr[rsp + reg_zp_comp_b_offs_]);
        mov(ptr[rsp + reg_aux_zp_comp_b_offs_], reg_zp_comp_b);
    }
}

template <typename Wmm>
void jit_brgemm_kernel_t<Wmm>::read_params() {
    Label label_done;

    if (brg.with_binary) mov(ptr[rsp + abi_param1_offs_], param1);

    if (brg.type == brgemm_addr) {
        mov(reg_addr_batch, ptr[param1 + GET_OFF(batch)]);
    } else {
        if (brg.layout == brgemm_row_major) {
            mov(reg_A, ptr[param1 + GET_OFF(ptr_A)]);
            mov(reg_B, ptr[param1 + GET_OFF(ptr_B)]);
        } else {
            mov(reg_A, ptr[param1 + GET_OFF(ptr_B)]);
            mov(reg_B, ptr[param1 + GET_OFF(ptr_A)]);
        }

        if (brg.type == brgemm_offs) {
            mov(reg_offs_batch, ptr[param1 + GET_OFF(batch)]);
            mov(ptr[rsp + origin_offs_batch_offs_], reg_offs_batch);
        } else {
            mov(reg_strd_batch, ptr[param1 + GET_OFF(batch)]);
            mov(ptr[rsp + origin_strd_batch_offs_], reg_strd_batch);
        }
    }

    mov(reg_C, ptr[param1 + GET_OFF(ptr_C)]);
    mov(reg_D, ptr[param1 + GET_OFF(ptr_D)]);
    mov(reg_BS, ptr[param1 + GET_OFF(BS)]);

    // ptr_buf is re-used for passing compensations for
    // brg.req_s8s8_compensation case
    if (brg.is_tmm || brg.req_s8s8_compensation) {
        mov(reg_buf, ptr[param1 + GET_OFF(ptr_buf)]);
        mov(ptr[rsp + reg_buf_offs_], reg_buf);
    }

    if (brg.with_bias) {
        mov(reg_bias, ptr[param1 + GET_OFF(ptr_bias)]);
        mov(ptr[rsp + reg_bias_offs_], reg_bias);
    }
    if (brg.with_scales) {
        mov(reg_scales, ptr[param1 + GET_OFF(ptr_scales)]);
        mov(ptr[rsp + reg_scales_offs_], reg_scales);
    }

    if (brg.zp_type_a != brgemm_broadcast_t::none) {
        mov(reg_zp_comp_a, ptr[param1 + GET_OFF(a_zp_compensations)]);
        mov(ptr[rsp + reg_zp_comp_a_offs_], reg_zp_comp_a);
    }

    if (brg.zp_type_b != brgemm_broadcast_t::none) {
        mov(reg_zp_comp_b, ptr[param1 + GET_OFF(b_zp_compensations)]);
        mov(ptr[rsp + reg_zp_comp_b_offs_], reg_zp_comp_b);
    }

    if (brg.with_wei_decomp) {
        mov(reg_wei_scales, ptr[param1 + GET_OFF(ptr_wei_scales)]);
        mov(ptr[rsp + reg_wei_scales_offs_], reg_wei_scales);

        mov(reg_wei_zp, ptr[param1 + GET_OFF(ptr_wei_zero_points)]);
        mov(ptr[rsp + reg_wei_zero_points_offs_], reg_wei_zp);

        mov(reg_ic, ptr[param1 + GET_OFF(ic)]);
        mov(ptr[rsp + reg_ic_offs_], reg_ic);
    }

    if (brg.with_src_dyn_quant) {
        mov(reg_src_scales, ptr[param1 + GET_OFF(ptr_src_scales)]);
        mov(ptr[rsp + reg_src_scales_offs_], reg_src_scales);
    }

    if (brg.zp_type_c != brgemm_broadcast_t::none) {
        mov(reg_zp_c_values, ptr[param1 + GET_OFF(c_zp_values)]);
        mov(ptr[rsp + reg_zp_c_values_offs_], reg_zp_c_values);
    }

    if (brg.with_dst_scales) {
        mov(reg_dst_scales, ptr[param1 + GET_OFF(ptr_dst_scales)]);
        mov(ptr[rsp + reg_dst_scales_offs_], reg_dst_scales);
    }

    if (brg.is_runtime_ldc) {
        mov(reg_tmp_read_values, ptr[param1 + GET_OFF(dynamic_LDC)]);
        if (brg.typesize_C > 1) shl(reg_tmp_read_values, (brg.typesize_C >> 1));
        mov(ptr[rsp + reg_C_shift_bytes_offs_], reg_tmp_read_values);
    }

    if (brg.is_runtime_ldd) {
        mov(reg_tmp_read_values, ptr[param1 + GET_OFF(dynamic_LDD)]);
        if (brg.typesize_D > 1) shl(reg_tmp_read_values, (brg.typesize_D >> 1));
        mov(ptr[rsp + reg_D_shift_bytes_offs_], reg_tmp_read_values);
    }

    mov(reg_do_post_ops, ptr[param1 + GET_OFF(do_post_ops)]);
    mov(ptr[rsp + reg_do_post_ops_offs_], reg_do_post_ops);

    mov(reg_skip_accm, ptr[param1 + GET_OFF(skip_accm)]);
    mov(ptr[rsp + reg_skip_accm_offs_], reg_skip_accm);

    mov(reg_zp_a_val, ptr[param1 + GET_OFF(zp_a_val)]);
    mov(ptr[rsp + reg_zp_a_val_offs_], reg_zp_a_val);

    mov(reg_do_comp, ptr[param1 + GET_OFF(do_apply_comp)]);
    mov(ptr[rsp + reg_do_comp_offs_], reg_do_comp);
}

template <typename Wmm>
void jit_brgemm_kernel_t<Wmm>::zero_accumulators(int bd_block2,
        bool is_bdb_tail, int ld_block2, bool is_ld_tail,
        bool skip_accumulation) {
    if (brg.is_tmm) {
        // avoid usage of tile registers if there is no accumulation
        if (skip_accumulation) return;
        for_(int bdb = 0; bdb < bd_block2; bdb++)
        for (int ldb = 0; ldb < ld_block2; ldb++) {
            int idx = (is_ld_tail) ? brg.ld_block2 : ldb;
            tilezero(Tmm(brg.get_C_tensor(bdb, idx, is_bdb_tail, is_ld_tail)));
        }
    } else {
        int bd_block = (is_bdb_tail) ? brg.bdb_tail : brg.bd_block;
        for_(int bd = 0; bd < bd_block; bd++)
        for (int ld = 0; ld < ld_block2; ld++) {
            auto vmm = accm(ld_block2, bd, ld);
            uni_vpxor(vmm, vmm, vmm);
        }
    }
}

// This method up-converts the data from bf8 to f16 and saves at reg_buf.
// Generally used by matrix_A, where no vnni transformation of data is needed.
template <typename Wmm>
void jit_brgemm_kernel_t<Wmm>::fp8_to_f16_upconvert(int num_rows,
        int tile_num_col_bytes, reg64_t reg_base, int offset,
        reg64_t reg_data_stride, data_type_t dt, bool is_rd_tail) {

    int rd_block = is_rd_tail ? brg.rdb_tail : brg.rd_block;

    const int max_num_cols = rd_block; //tile_num_col_bytes / sizeof(float16_t);
    const int col_tail = max_num_cols % 32;
    auto zmm_1 = vmm_tmp(0);
    auto zmm_1_masked = col_tail ? zmm_1 | fp8_col_mask | T_z : zmm_1;

    assert(max_num_cols > 0);

    if (col_tail) {
        const int tail_mask = (1 << col_tail) - 1;
        auto reg_tmp_32 = reg_tmp_gpr.cvt32();
        mov(reg_tmp_32, tail_mask);
        kmovd(fp8_col_mask, reg_tmp_32);
    }
    // Note: using the same register used in col_tail, so order is important
    const auto reg_data_aux = reg_tmp_gpr;
    lea(reg_data_aux, ptr[reg_base + offset]);

    for (int r = 0; r < num_rows; ++r) {
        if (dt == data_type::f8_e5m2)
            f8_e5m2_emulator_->vcvt_f8_to_f16(zmm_1_masked, ptr[reg_data_aux]);
        else if (dt == data_type::f8_e4m3)
            f8_e4m3_emulator_->vcvt_f8_to_f16(zmm_1_masked, ptr[reg_data_aux]);
        else
            assert(!"unsupported data type");

        vmovups(ptr[reg_buf_aux + r * zmm_width_in_bytes_], zmm_1);
        add(reg_data_aux, reg_data_stride);
    }
}

// This method up-converts and transforms the data from fp8_vnni to f16_vnni
// format. Generally used by matrix_B.
template <typename Wmm>
void jit_brgemm_kernel_t<Wmm>::fp8_to_f16_upconvert_to_vnni(int num_rows,
        int tile_num_col_bytes, reg64_t reg_base, int offset,
        reg64_t reg_data_stride, data_type_t dt, bool is_rd_tail) {
    const int num_cols_ele = tile_num_col_bytes / 2; // 32 for full tile
    const int num_N = num_cols_ele / 2; // 16 for full tile
    const auto zmm_2 = vmm_tmp(2);

    assert(num_N > 0 && "bad tile parameters");
    MAYBE_UNUSED(num_N);

    const auto reg_data_aux = reg_tmp_gpr;
    lea(reg_data_aux, ptr[reg_base + offset]);

    int rd_block = is_rd_tail ? brg.rdb_tail : brg.rd_block;
    const int vnni_granularity = data_type_vnni_granularity(data_type::f16);
    const int r_end = utils::div_up(rd_block, vnni_granularity);
    assert(r_end <= num_rows && "bad tile parameters");

    if (dt == data_type::f8_e5m2)
        f8_e5m2_emulator_->vcvt_f8_to_f16_vnni_block(
                r_end, reg_data_aux, reg_data_stride, reg_buf_aux);
    else if (dt == data_type::f8_e4m3)
        f8_e4m3_emulator_->vcvt_f8_to_f16_vnni_block(
                r_end, reg_data_aux, reg_data_stride, reg_buf_aux);
    else
        assert(!"unsupported data type");

    // zero rest of the tile data
    if (r_end < num_rows) {
        vpxord(zmm_2, zmm_2, zmm_2);
        for (int r = r_end; r < num_rows; ++r)
            vmovups(ptr[reg_buf_aux + r * zmm_width_in_bytes_], zmm_2);
    }
}

template <typename Wmm>
void jit_brgemm_kernel_t<Wmm>::apply_alpha_beta(
        int bd_block, int ld_block2, bool is_ld_tail) {
    const bool apply_alpha = brg.alpha != 1.f;
    const bool dq2ps_required = brg.is_int8 && (apply_alpha || brg.beta != 1.f) && !brg.with_src_dyn_quant;

    auto vmm_alpha = vmm_tmp(0);
    if (apply_alpha) {
        mov(reg_tmp_gpr, float2int(static_cast<float>(brg.alpha)));
        uni_vmovq(Xmm(vmm_alpha.getIdx()), reg_tmp_gpr);
        uni_vbroadcastss(vmm_alpha, Xmm(vmm_alpha.getIdx()));
    }
    for_(int bd = 0; bd < bd_block; bd++)
    for (int ld = 0; ld < ld_block2; ld++) {
        auto vmm = accm(ld_block2, bd, ld);
        if (dq2ps_required) uni_vcvtdq2ps(vmm, vmm);
        if (apply_alpha) uni_vmulps(vmm, vmm, vmm_alpha);
    }

    if (brg.beta == 0.f) return;
    const bool use_vadd_for_beta = brg.beta == 1.f && !dq2ps_required;
    const bool need_init_beta_vmm = brg.beta != 1.f;
    auto vmm_prev_dst = vmm_tmp(0);
    auto vmm_beta = vmm_tail_mask();
    if (need_init_beta_vmm) {
        mov(reg_tmp_gpr, float2int(static_cast<float>(brg.beta)));
        uni_vmovq(Xmm(vmm_beta.getIdx()), reg_tmp_gpr);
        uni_vbroadcastss(vmm_beta, Xmm(vmm_beta.getIdx()));
    }

    if (brg.is_runtime_ldc && bd_block > 1)
        mov(ptr[rsp + reg_aux_C_backup_offs_], reg_aux_C);

    for_(int bd = 0; bd < bd_block; bd++)
    for (int ld = 0; ld < ld_block2; ld++) {
        const bool is_tail = is_ld_tail && ld + 1 == ld_block2;
        const auto k_mask = is_tail ? ld_tail_mask : ld_full_mask;
        auto vmm = accm(ld_block2, bd, ld);
        auto ptr_C = ptr[reg_aux_C + C_offset(bd, ld)];
        if (use_vadd_for_beta) {
            if (IMPLICATION(is_tail, is_superset(brg.isa_impl, avx512_core))) {
                auto vmm_masked = vmm_mask(vmm, is_tail, false, k_mask);
                if (brg.is_int8 && !brg.with_src_dyn_quant)
                    uni_vpaddd(vmm_masked, vmm, ptr_C);
                else
                    uni_vaddps(vmm_masked, vmm, ptr_C);
            } else {
                vmaskmovps(vmm_prev_dst, vmm_tail_mask(), ptr_C);
                if (brg.is_int8)
                    uni_vpaddd(vmm, vmm, vmm_prev_dst);
                else
                    uni_vaddps(vmm, vmm, vmm_prev_dst);
            }
        } else {
            const int ld_size = is_tail ? brg.ldb_tail : brg.ld_block;
            cvt2ps(brg.dt_c, vmm_prev_dst, ptr_C, is_tail, false, k_mask,
                    ld_size);
            if (brg.beta == 1.f)
                uni_vaddps(vmm, vmm, vmm_prev_dst);
            else
                uni_vfmadd231ps(vmm, vmm_prev_dst, vmm_beta);
        }
        if (brg.is_runtime_ldc && bd_block > 1 && ld == ld_block2 - 1)
            add(reg_aux_C, ptr[rsp + reg_C_shift_bytes_offs_]);
    }

    if (brg.is_runtime_ldc && bd_block > 1)
        mov(reg_aux_C, ptr[rsp + reg_aux_C_backup_offs_]);

    if (need_init_beta_vmm) maybe_set_avx_mask(is_ld_tail);
}

template <typename Wmm>
void jit_brgemm_kernel_t<Wmm>::apply_post_ops(
        int bd_block, int ld_block2, int ldb_and_bdb_offset, bool is_ld_tail) {

    binary_injector::rhs_arg_dynamic_params_t rhs_arg_params;

    const injector_utils::conditional_register_preserve_guard_t register_guard(
            brg.with_binary, this, {param1});
    const auto guard_space = register_guard.stack_space_occupied();
    if (brg.with_binary) {
        mov(param1, ptr[rsp + abi_param1_offs_ + guard_space]);
    }

    if (brg.is_runtime_ldd && bd_block > 1)
        mov(ptr[rsp + reg_aux_D_backup_offs_], reg_aux_D);

    const int bd_block_shift = brg.is_runtime_ldd ? 1 : bd_block;
    for (int bd_block_idx = 0; bd_block_idx < bd_block;
            bd_block_idx += bd_block_shift) {
        int bd_start = bd_block_idx;
        int bd_end = bd_start + bd_block_shift;

        const auto set_binary_injecotr_params = [&] {
            if (!brg.with_binary || !with_binary_non_scalar_bcast_) return;
            for_(int bd = bd_start; bd < bd_end; bd++)
            for (int ld = 0; ld < ld_block2; ld++) {
                const auto vmm_idx = accm(ld_block2, bd, ld).getIdx();

                rhs_arg_params.vmm_idx_to_out_reg.emplace(vmm_idx, reg_aux_D);
                rhs_arg_params.vmm_idx_to_out_elem_off_val.emplace(
                        vmm_idx, D_offset(bd, ld));
                if (is_ld_tail) rhs_arg_params.vmm_tail_idx_.emplace(vmm_idx);
            }
        };

        const auto sum_injector = [&] {
            const float *p_sum_scale = &brg.sum_scale;
            const int32_t *p_sum_zp = &brg.sum_zp;
            const bool p_sum_scale_reg_set = *p_sum_scale != 1.f;
            const bool p_sum_zp_reg_set = *p_sum_zp != 0;
            const bool reset_avx_tail_mask = p_sum_zp_reg_set;

            {
                const injector_utils::conditional_register_preserve_guard_t
                        register_guard_sum_scale((with_binary_non_scalar_bcast_)
                                        && p_sum_scale_reg_set,
                                this, {reg_ptr_sum_scale});
                const injector_utils::conditional_register_preserve_guard_t
                        register_guard_sum_zp(
                                p_sum_zp_reg_set, this, {reg_ptr_sum_zp});

                const auto &vmm_sum_zp = vmm_tmp(1);

                if (p_sum_zp_reg_set) {
                    mov(reg_ptr_sum_zp, reinterpret_cast<size_t>(p_sum_zp));
                    if (is_superset(brg.isa_impl, avx512_core)) {
                        vcvtdq2ps(vmm_sum_zp, ptr_b[reg_ptr_sum_zp]);
                    } else {
                        uni_vpbroadcastd(vmm_sum_zp, ptr[reg_ptr_sum_zp]);
                        uni_vcvtdq2ps(vmm_sum_zp, vmm_sum_zp);
                    }
                }

                if (p_sum_scale_reg_set) {
                    if (is_superset(brg.isa_impl, avx512_core)) {
                        // embd bcast fma
                        mov(reg_ptr_sum_scale,
                                reinterpret_cast<size_t>(p_sum_scale));
                    } else {
                        lea(reg_ptr_sum_scale, ptr[rip + sum_zp_scale_data_]);
                    }
                }

                for_(int bd = bd_start; bd < bd_end; bd++)
                for (int ld = 0; ld < ld_block2; ld++) {
                    const auto vmm = accm(ld_block2, bd, ld);
                    const auto addr = ptr[reg_aux_D + D_offset(bd, ld)];
                    const auto vmm_prev_dst = vmm_tmp(0);
                    const bool is_tail = is_ld_tail && ld + 1 == ld_block2;
                    const auto k_mask = is_tail ? ld_tail_mask : ld_full_mask;
                    const int ld_size = is_tail ? brg.ldb_tail : brg.ld_block;
                    cvt2ps(brg.sum_dt, vmm_prev_dst, addr, is_tail, false,
                            k_mask, ld_size);
                    if (p_sum_zp_reg_set)
                        uni_vsubps(vmm_prev_dst, vmm_prev_dst, vmm_sum_zp);
                    if (p_sum_scale_reg_set) {
                        if (is_superset(brg.isa_impl, avx512_core))
                            uni_vfmadd231ps(vmm, vmm_prev_dst,
                                    ptr_b[reg_ptr_sum_scale]);
                        else
                            uni_vfmadd231ps(
                                    vmm, vmm_prev_dst, ptr[reg_ptr_sum_scale]);
                    } else
                        uni_vaddps(vmm, vmm, vmm_prev_dst);
                }
            }

            if (reset_avx_tail_mask) maybe_set_avx_mask(is_ld_tail);
        };

        set_binary_injecotr_params();

        if (brg.with_sum) {
            postops_injector_->set_lambda_injector(
                    primitive_kind::sum, sum_injector);
        }

        postops_injector_->compute_vector_range(
                max_effective_vregs - bd_end * ld_block2,
                max_effective_vregs - bd_start * ld_block2, rhs_arg_params);

        if (brg.is_runtime_ldd && bd_block > 1)
            add(reg_aux_D, ptr[rsp + reg_D_shift_bytes_offs_]);
    }

    if (brg.is_runtime_ldd && bd_block > 1)
        mov(reg_aux_D, ptr[rsp + reg_aux_D_backup_offs_]);
}

template <typename Wmm>
void jit_brgemm_kernel_t<Wmm>::store_accumulators_apply_post_ops(
        int bd_block, int ld_block2, int ldb_and_bdb_offset, bool is_ld_tail) {
    auto k_mask = (!is_ld_tail) ? ld_full_mask : ld_tail_mask;

    // if (brg.is_int8 && alpha_or_beta_applicable && !beta_uses_vadd) ->
    // accumulated values are already converted to ps in apply_alpha_beta()
    const bool alpha_or_beta_applicable = brg.alpha != 1.0f || brg.beta != 0.f;
    const bool beta_uses_vadd
            = brg.beta == 1.f && IMPLICATION(brg.is_int8, brg.alpha == 1.0f);
    const bool dq2ps_required = brg.is_int8
            && IMPLICATION(alpha_or_beta_applicable, beta_uses_vadd)
            && !brg.with_src_dyn_quant;

    if (brg.with_scales) {
        mov(reg_aux_scales, ptr[rsp + reg_aux_scales_offs_]);
        for (int ld = 0; ld < ld_block2; ld++) {
            const auto addr = ptr[reg_aux_scales + scales_offset(ld)];
            const bool is_tail = is_ld_tail && ld + 1 == ld_block2;
            auto vmm_scales = vmm_tmp(0);
            if (IMPLICATION(is_tail, isa_has_masks(brg.isa_impl))) {
                const Vmm vmm_masked
                        = vmm_mask(vmm_scales, is_tail, false, k_mask);
                uni_vmovups(vmm_masked, addr);
            } else {
                auto vmm_scales = vmm_tmp(0);
                vmaskmovps(vmm_scales, vmm_tail_mask(), addr);
            }
            for (int bd = 0; bd < bd_block; bd++) {
                auto vmm = accm(ld_block2, bd, ld);
                if (dq2ps_required) uni_vcvtdq2ps(vmm, vmm);
                uni_vmulps(vmm, vmm, vmm_scales);
            }
        }
    }

    if (brg.with_bias) { mov(reg_aux_bias, ptr[rsp + reg_aux_bias_offs_]); }
    for (int ld = 0; ld < ld_block2; ld++) {
        auto vmm_bias = vmm_tmp(0);
        if (brg.with_bias) {
            auto ptr_bias = ptr[reg_aux_bias + bias_offset(ld)];
            const bool is_tail = is_ld_tail && ld + 1 == ld_block2;
            cvt2ps(brg.dt_bias, vmm_bias, ptr_bias, is_tail, false, k_mask,
                    is_tail ? brg.ldb_tail : brg.ld_block);
        }
        for (int bd = 0; bd < bd_block; bd++) {
            auto vmm = accm(ld_block2, bd, ld);
            if (dq2ps_required && !brg.with_scales) uni_vcvtdq2ps(vmm, vmm);
            if (brg.with_bias) uni_vaddps(vmm, vmm, vmm_bias);
        }
    }

    if (postops_injector_)
        apply_post_ops(bd_block, ld_block2, ldb_and_bdb_offset, is_ld_tail);

    if (brg.with_dst_scales) {
        mov(reg_aux_dst_scales, ptr[rsp + reg_dst_scales_offs_]);
        auto vmm_dst_scales = vmm_tmp(0);
        vbroadcastss(vmm_dst_scales, ptr[reg_aux_dst_scales]);

        for (int ld = 0; ld < ld_block2; ld++) {
            for (int bd = 0; bd < bd_block; bd++) {
                auto vmm = accm(ld_block2, bd, ld);
                vmulps(vmm, vmm, vmm_dst_scales);
            }
        }
    }

    if (brg.zp_type_c != brgemm_broadcast_t::none) {
        mov(reg_aux_zp_c_values, ptr[rsp + reg_aux_zp_c_values_offs_]);
        auto vmm_zp_c = vmm_tmp(0);
        if (brg.zp_type_c == brgemm_broadcast_t::per_tensor) {
            if (is_superset(brg.isa_impl, avx512_core)) {
                uni_vcvtdq2ps(vmm_zp_c,
                        EVEX_compress_addr(reg_aux_zp_c_values, 0, true));
            } else {
                uni_vpbroadcastd(vmm_zp_c, ptr[reg_aux_zp_c_values]);
                uni_vcvtdq2ps(vmm_zp_c, vmm_zp_c);
            }
        }
        for (int ld = 0; ld < ld_block2; ld++) {
            const bool is_tail = is_ld_tail && ld + 1 == ld_block2;
            if (brg.zp_type_c == brgemm_broadcast_t::per_n) {
                int zp_c_off = zp_c_values_offset(ld);
                if (is_superset(brg.isa_impl, avx512_core)) {
                    auto zp_c_addr
                            = EVEX_compress_addr(reg_aux_zp_c_values, zp_c_off);
                    cvt2ps(data_type::s32, vmm_zp_c, zp_c_addr, is_tail, false,
                            k_mask, is_tail ? brg.ldb_tail : brg.ld_block);
                } else {
                    cvt2ps(data_type::s32, vmm_zp_c,
                            ptr[reg_aux_zp_c_values + zp_c_off], is_tail, false,
                            k_mask, is_tail ? brg.ldb_tail : brg.ld_block);
                }
            }
            for (int bd = 0; bd < bd_block; bd++) {
                auto vmm = accm(ld_block2, bd, ld);
                uni_vaddps(vmm, vmm, vmm_zp_c);
            }
        }
    }

    const bool dt_requires_saturation
            = one_of(brg.dt_d, data_type::u8, data_type::s8, data_type::s32);
    auto vmm_lbound = vmm_tail_mask();
    auto vmm_ubound = vmm_tmp(0);
    assert(vmm_lbound.getIdx() != vmm_ubound.getIdx());
    if (dt_requires_saturation) {
        init_saturate_f32(
                vmm_lbound, vmm_ubound, reg_tmp_gpr, data_type::f32, brg.dt_d);
        for (int bd = 0; bd < bd_block; bd++) {
            for (int ld = 0; ld < ld_block2; ld++) {
                auto vmm = accm(ld_block2, bd, ld);
                saturate_cvt_f32(vmm, vmm_lbound, vmm_ubound, brg.dt_d);
            }
        }
        // below call is not required as s32 doesn't use vmm_lbound
        // maybe_set_avx_mask(is_ld_tail);
    }

    if (brg.is_bf16_emu) bf16_emu_->init_vcvtneps2bf16();

    if (brg.is_runtime_ldd && bd_block > 1)
        mov(ptr[rsp + reg_aux_D_backup_offs_], reg_aux_D);

    if (brg.is_fp8_via_convert()) mov(ptr[rsp + reg_val_tmp_1_], reg64_fp8_aux);
    for_(int bd = 0; bd < bd_block; bd++)
    for (int ld = 0; ld < ld_block2; ld++) {
        auto addr = ptr[reg_aux_D + D_offset(bd, ld)];
        auto vmm = accm(ld_block2, bd, ld);
        auto vmm_lower = Vmm_lower_t(vmm.getIdx());
        const bool is_tail = is_ld_tail && ld + 1 == ld_block2;
        if (is_superset(brg.isa_impl, avx512_core)) {
            const Vmm r_vmm = vmm_mask(vmm, is_tail, true, k_mask);
            const Vmm_lower_t r_ymm
                    = vmm_lower_mask(vmm_lower, is_tail, true, k_mask);
            const Xmm xmm = Xmm(vmm.getIdx());
            const Xmm r_xmm = is_tail ? xmm | k_mask : xmm;
            switch (brg.dt_d) {
                case data_type::f32:
                case data_type::s32: uni_vmovups(addr, r_vmm); break;
                case data_type::bf16: // TODO - clean
                    if (brg.is_bf16_emu) {
                        bf16_emu_->vcvtneps2bf16(vmm_lower, vmm);
                    } else {
                        vcvtneps2bf16(vmm_lower, vmm);
                    }
                    vmovdqu16(addr, r_ymm);
                    break;
                case data_type::f16:
                    vcvtps2ph(vmm_lower, vmm, _op_mxcsr);
                    vmovdqu16(addr, r_ymm);
                    break;
                case data_type::f8_e5m2:
                    if (brg.is_fp8_via_convert()) {
                        f8_e5m2_emulator_->vcvt_f32_to_f8(xmm, vmm);
                        vmovdqu8(addr, r_xmm);
                    } else
                        assert(!"Error, native conversion unsupported");
                    break;
                case data_type::f8_e4m3:
                    if (brg.is_fp8_via_convert()) {
                        f8_e4m3_emulator_->vcvt_f32_to_f8(xmm, vmm);
                        vmovdqu8(addr, r_xmm);
                    } else
                        assert(!"Error, native conversion unsupported");
                    break;
                case data_type::s8: vpmovsdb(addr, r_vmm); break;
                case data_type::u8: vpmovusdb(addr, r_vmm); break;
                default: assert(!"unknown dst_dt");
            }
        } else {
            const int ld_block = is_tail ? brg.ldb_tail : brg.ld_block;
            if (is_tail && types::data_type_size(brg.dt_b) == sizeof(float))
                vmaskmovps(addr, vmm_tail_mask(), vmm);
            else
                store_data(
                        brg.dt_d, vmm, reg_aux_D, D_offset(bd, ld), ld_block);
        }
        if (brg.is_runtime_ldd && bd_block > 1 && ld == ld_block2 - 1)
            add(reg_aux_D, ptr[rsp + reg_D_shift_bytes_offs_]);
    }
    if (brg.is_fp8_via_convert()) mov(reg64_fp8_aux, ptr[rsp + reg_val_tmp_1_]);

    if (brg.is_runtime_ldd && bd_block > 1)
        mov(reg_aux_D, ptr[rsp + reg_aux_D_backup_offs_]);
}

template <typename Wmm>
void jit_brgemm_kernel_t<Wmm>::apply_compensation(
        int bd_block, int ld_block2, bool is_ld_tail) {
    // apply compensation to accumulated values
    // to avoid the loss of accuracy when converting s32 to f32
    auto k_mask = (!is_ld_tail) ? ld_full_mask : ld_tail_mask;

    if (!brg.req_cal_comp_pads && brg.zp_type_a != brgemm_broadcast_t::none) {
        auto vmm_zp_a_val = vmm_tmp(1);
        mov(reg_zp_a_val, ptr[rsp + reg_zp_a_val_offs_]);
        uni_vpbroadcastd(vmm_zp_a_val, reg_zp_a_val.cvt32());

        mov(reg_aux_zp_comp_a, ptr[rsp + reg_aux_zp_comp_a_offs_]);
        const auto vmm_zp_comp_a = vmm_tmp(0);
        for (int ld = 0; ld < ld_block2; ld++) {
            const bool is_tail = is_ld_tail && ld + 1 == ld_block2;
            for (int bd = 0; bd < bd_block; bd++) {
                if (IMPLICATION(!brg.req_comp_pads_with_bcast, bd == 0)) {
                    const auto zp_comp_a_addr = ptr[reg_aux_zp_comp_a
                            + bd_zp_comp_a_offset(ld, bd)];
                    if (IMPLICATION(is_tail, isa_has_masks(brg.isa_impl))) {
                        auto vmm_zp_comp_a_masked = vmm_mask(
                                vmm_zp_comp_a, is_tail, false, k_mask);
                        uni_vmovups(vmm_zp_comp_a_masked, zp_comp_a_addr);
                    } else {
                        // cannot use vmaskmovps as vmm_zp_a_val clashes with
                        // vmm_tail_mask
                        load_data(data_type::s32, vmm_zp_comp_a, zp_comp_a_addr,
                                brg.ldb_tail);
                    }
                    uni_vpmulld(vmm_zp_comp_a, vmm_zp_comp_a, vmm_zp_a_val);
                }
                auto vmm = accm(ld_block2, bd, ld);
                uni_vpaddd(vmm, vmm, vmm_zp_comp_a);
            }
        }
        maybe_set_avx_mask(is_ld_tail);
    }

    if (brg.zp_type_b != brgemm_broadcast_t::none) {
        mov(reg_aux_zp_comp_b, ptr[rsp + reg_aux_zp_comp_b_offs_]);
        for (int bd = 0; bd < bd_block; bd++) {
            int zp_comp_b_off = zp_comp_b_offset(bd);
            for (int ld = 0; ld < ld_block2; ld++) {
                auto vmm = accm(ld_block2, bd, ld);
                if (is_superset(brg.isa_impl, avx512_core)) {
                    const auto zp_comp_b_addr = EVEX_compress_addr(
                            reg_aux_zp_comp_b, zp_comp_b_off, true);
                    uni_vpaddd(vmm, vmm, zp_comp_b_addr);
                } else {
                    const auto vmm_zp_comp_b = vmm_tmp(0);
                    uni_vpbroadcastd(vmm_zp_comp_b,
                            ptr[reg_aux_zp_comp_b + zp_comp_b_off]);
                    uni_vpaddd(vmm, vmm, vmm_zp_comp_b);
                }
            }
        }
    }

    if (!brg.req_cal_comp_pads && brg.req_s8s8_compensation) {
        mov(reg_aux_compensation, ptr[rsp + reg_aux_comp_offs_]);
        auto vmm_comp = vmm_tmp(0);
        for (int ld = 0; ld < ld_block2; ld++) {
            const bool is_tail = is_ld_tail && ld + 1 == ld_block2;
            for (int bd = 0; bd < bd_block; bd++) {
                if (IMPLICATION(!brg.req_comp_pads_with_bcast, bd == 0)) {
                    const auto comp_addr = ptr[reg_aux_compensation
                            + bd_compensation_offset(ld, bd)];
                    if (IMPLICATION(is_tail,
                                is_superset(brg.isa_impl, avx512_core))) {
                        auto vmm_comp_masked
                                = vmm_mask(vmm_comp, is_tail, false, k_mask);
                        uni_vmovups(vmm_comp_masked, comp_addr);
                    } else
                        vmaskmovps(vmm_comp, vmm_tail_mask(), comp_addr);
                }
                auto vmm = accm(ld_block2, bd, ld);
                uni_vpaddd(vmm, vmm, vmm_comp);
            }
        }
    }
}

template <typename Wmm>
void jit_brgemm_kernel_t<Wmm>::store_accumulators_without_post_ops(
        int bd_block, int ld_block2, bool is_ld_tail) {

    // if (brg.is_int8 && alpha_or_beta_applicable && !beta_uses_vadd) ->
    // accumulated values are converted to ps in apply_alpha_beta()
    const bool alpha_or_beta_applicable = brg.alpha != 1.0f || brg.beta != 0.f;
    const bool beta_uses_vadd
            = brg.beta == 1.f && IMPLICATION(brg.is_int8, brg.alpha == 1.0f);
    const bool dt_requires_saturation = brg.is_int8
            && !IMPLICATION(alpha_or_beta_applicable, beta_uses_vadd);

    if (dt_requires_saturation) {
        auto vmm_ubound = vmm_tmp(0);
        auto vmm_lbound = vmm_tmp(1);
        init_saturate_f32(
                vmm_lbound, vmm_ubound, reg_tmp_gpr, data_type::f32, brg.dt_d);
        for (int bd = 0; bd < bd_block; bd++) {
            for (int ld = 0; ld < ld_block2; ld++) {
                auto vmm = accm(ld_block2, bd, ld);
                saturate_cvt_f32(vmm, vmm_lbound, vmm_ubound, brg.dt_d);
            }
        }
        // below call is not required as s32 doesn't use vmm_lbound
        // maybe_set_avx_mask(is_ld_tail);
    }

    if (brg.is_runtime_ldc && bd_block > 1)
        mov(ptr[rsp + reg_aux_C_backup_offs_], reg_aux_C);

    for_(int bd = 0; bd < bd_block; bd++)
    for (int ld = 0; ld < ld_block2; ld++) {
        auto vmm = accm(ld_block2, bd, ld);
        const auto addr_c = ptr[reg_aux_C + C_offset(bd, ld)];
        const bool is_tail = is_ld_tail && ld + 1 == ld_block2;
        if (!is_tail)
            uni_vmovups(addr_c, vmm);
        else if (isa_has_masks(brg.isa_impl)) { // is_tail
            uni_vmovups(addr_c | ld_tail_mask | T_z, vmm);
        } else {
            vmaskmovps(addr_c, vmm_tail_mask(), vmm);
        }
        if (brg.is_runtime_ldc && bd_block > 1 && ld == ld_block2 - 1)
            add(reg_aux_C, ptr[rsp + reg_C_shift_bytes_offs_]);
    }

    if (brg.is_runtime_ldc && bd_block > 1)
        mov(reg_aux_C, ptr[rsp + reg_aux_C_backup_offs_]);
}

template <typename Wmm>
void jit_brgemm_kernel_t<Wmm>::store_accumulators(int bd_block2,
        bool is_bdb_tail, int ld_block2, bool is_ld_tail,
        bool skip_accumulation) {
    const bool has_zero_points = !everyone_is(brgemm_broadcast_t::none,
            brg.zp_type_a, brg.zp_type_b, brg.zp_type_c);
    const bool are_post_ops_applicable = one_of(true, brg.with_eltwise,
            brg.with_binary, brg.with_scales, brg.with_bias, brg.with_sum,
            brg.dt_d != brg.dt_c, brg.req_s8s8_compensation, has_zero_points,
            brg.with_dst_scales);
    const bool need_to_apply_alpha_beta = brg.beta != 0.f || brg.alpha != 1.f;
    const bool need_generate_zp_a_compensation
            = brg.is_int8 && (brg.req_s8s8_compensation || has_zero_points);

    maybe_set_avx_mask(is_ld_tail);

    if (brg.is_tmm) {
        if (need_to_apply_alpha_beta || are_post_ops_applicable
                || need_generate_zp_a_compensation)
            mov(reg_stride_ld_block, brg.ld_block * brg.typesize_C);
        else if (brg.is_runtime_ldc)
            mov(reg_stride_ld_block, ptr[rsp + reg_C_shift_bytes_offs_]);
        else
            mov(reg_stride_ld_block, brg.LDC * brg.typesize_C);

        auto store_accumulators_amx = [&](const bool apply_post_ops,
                                              const bool apply_zp_a_compensation
                                              = false) {
            mov(ptr[rsp + reg_aux_C_bdb_loop_backup_offs_], reg_aux_C);
            if (brg.is_runtime_ldc && bd_block2 > 1) {
                xor_(reg_buf, reg_buf);
                imul(reg_buf, ptr[rsp + reg_C_shift_bytes_offs_],
                        bdb_C_offset(1));
                mov(ptr[rsp + reg_aux_C_bdb_loop_shift_offs_], reg_buf);
            }

            if (apply_post_ops) {
                mov(ptr[rsp + reg_aux_D_bdb_loop_backup_offs_], reg_aux_D);
                if (brg.is_runtime_ldd && bd_block2 > 1) {
                    xor_(reg_buf, reg_buf);
                    imul(reg_buf, ptr[rsp + reg_D_shift_bytes_offs_],
                            bdb_D_offset(1));
                    mov(ptr[rsp + reg_aux_D_bdb_loop_shift_offs_], reg_buf);
                }
            }

            mov(reg_buf, ptr[rsp + reg_buf_offs_]);
            for (int bdb = 0; bdb < bd_block2; bdb++) {
                int adj_bd_block = (brg.is_M_tail && is_bdb_tail)
                        ? brg.bdb_tail
                        : brg.bd_block;
                for (int ldb = 0; ldb < ld_block2; ldb++) {
                    int idx = (is_ld_tail) ? brg.ld_block2 : ldb;
                    if (need_to_apply_alpha_beta || are_post_ops_applicable
                            || apply_zp_a_compensation) {
                        if (skip_accumulation) {
                            for (int bd = 0; bd < adj_bd_block; bd++) {
                                auto vreg_acc = accm(1, bd, 0);
                                uni_vpxor(vreg_acc, vreg_acc, vreg_acc);
                            }
                        } else {
                            tilestored(ptr[reg_buf + reg_stride_ld_block],
                                    Tmm(brg.get_C_tensor(bdb, idx, is_bdb_tail,
                                            is_ld_tail)));
                            for (int bd = 0; bd < adj_bd_block; bd++) {
                                size_t buf_offset
                                        = (bd * brg.ld_block) * brg.typesize_C;
                                auto vreg_acc = is_ld_tail
                                        ? accm(1, bd, 0) | ld_tail_mask | T_z
                                        : accm(1, bd, 0);
                                uni_vmovups(
                                        vreg_acc, ptr[reg_buf + buf_offset]);
                            }
                        }

                        if (apply_zp_a_compensation) {
                            apply_compensation(adj_bd_block, 1, is_ld_tail);
                        }

                        if (need_to_apply_alpha_beta)
                            apply_alpha_beta(adj_bd_block, 1, is_ld_tail);

                        if (apply_post_ops) {
                            const size_t ldb_and_bdb_offset
                                    = ldb_po_offset(ldb) + bdb_po_offset(bdb);
                            store_accumulators_apply_post_ops(adj_bd_block, 1,
                                    ldb_and_bdb_offset, is_ld_tail);
                            if (ldb < ld_block2 - 1) {
                                advance_ldb_post_op_regs();
                                add(reg_aux_D, ldb_D_offset(1));
                            }
                        } else {
                            store_accumulators_without_post_ops(
                                    adj_bd_block, 1, is_ld_tail);
                        }
                        mov(reg_buf, ptr[rsp + reg_buf_offs_]);
                    } else {
                        auto tmm = Tmm(brg.get_C_tensor(
                                bdb, idx, is_bdb_tail, is_ld_tail));
                        if (skip_accumulation) tilezero(tmm);
                        tilestored(ptr[reg_aux_C + reg_stride_ld_block], tmm);
                    }
                    if (ldb < ld_block2 - 1) add(reg_aux_C, ldb_C_offset(1));
                }
                if (ld_block2 > 1) sub(reg_aux_C, ldb_C_offset(ld_block2 - 1));
                if (bdb < bd_block2 - 1) {
                    if (brg.is_runtime_ldc)
                        add(reg_aux_C,
                                ptr[rsp + reg_aux_C_bdb_loop_shift_offs_]);
                    else
                        add(reg_aux_C, bdb_C_offset(1));
                }

                if (apply_post_ops) {
                    bool post_processed = false;
                    if (ld_block2 > 1) {
                        sub(reg_aux_D, ldb_D_offset(ld_block2 - 1));
                        restore_ldb_post_op_regs(ld_block2);
                        post_processed |= utils::one_of(true, brg.with_bias,
                                brg.with_scales,
                                brg.zp_type_a != brgemm_broadcast_t::none,
                                brg.zp_type_c == brgemm_broadcast_t::per_n,
                                brg.with_dst_scales);
                    }
                    if (bdb < bd_block2 - 1) {
                        if (brg.is_runtime_ldd)
                            add(reg_aux_D,
                                    ptr[rsp + reg_aux_D_bdb_loop_shift_offs_]);
                        else
                            add(reg_aux_D, bdb_D_offset(1));

                        advance_bdb_post_op_regs(adj_bd_block);
                        post_processed |= utils::one_of(true,
                                brg.zp_type_b != brgemm_broadcast_t::none,
                                brg.req_comp_pads_with_bcast
                                        && brg.zp_type_a
                                                != brgemm_broadcast_t::none);
                    }
                    if (post_processed) mov(reg_buf, ptr[rsp + reg_buf_offs_]);
                }
            }
            mov(reg_aux_C, ptr[rsp + reg_aux_C_bdb_loop_backup_offs_]);
            if (apply_post_ops) {
                mov(reg_aux_D, ptr[rsp + reg_aux_D_bdb_loop_backup_offs_]);
                restore_bdb_post_op_regs(bd_block2);
            }
        };

        Label label_done;
        if (are_post_ops_applicable) {
            Label label_skip_post_ops;
            mov(reg_do_post_ops, ptr[rsp + reg_do_post_ops_offs_]);
            cmp(reg_do_post_ops, 0);
            jz(label_skip_post_ops, T_NEAR);
            if (need_generate_zp_a_compensation) {
                Label label_skip_zp_comp_with_postops;
                mov(reg_do_comp, ptr[rsp + reg_do_comp_offs_]);
                cmp(reg_do_comp, 0);
                jz(label_skip_zp_comp_with_postops, T_NEAR);
                store_accumulators_amx(true, true);
                jmp(label_done, T_NEAR);

                L_aligned(label_skip_zp_comp_with_postops);
            }
            store_accumulators_amx(true);

            jmp(label_done, T_NEAR);

            L_aligned(label_skip_post_ops);
        }

        if (need_generate_zp_a_compensation) {
            Label label_skip_zp_comp;
            mov(reg_do_comp, ptr[rsp + reg_do_comp_offs_]);
            cmp(reg_do_comp, 0);
            jz(label_skip_zp_comp, T_NEAR);
            store_accumulators_amx(false, true);
            jmp(label_done, T_NEAR);

            L_aligned(label_skip_zp_comp);
        }

        store_accumulators_amx(false);
        L_aligned(label_done);
    } else {
        int bd_block = (is_bdb_tail) ? brg.bdb_tail : brg.bd_block;

        if (need_generate_zp_a_compensation) {
            Label label_store_without_comp;
            mov(reg_do_comp, ptr[rsp + reg_do_comp_offs_]);
            cmp(reg_do_comp, 0);
            jz(label_store_without_comp, T_NEAR);
            apply_compensation(bd_block, ld_block2, is_ld_tail);

            L_aligned(label_store_without_comp);
        }

        if (need_to_apply_alpha_beta)
            apply_alpha_beta(bd_block, ld_block2, is_ld_tail);

        Label label_done;
        if (are_post_ops_applicable) {
            Label label_store_without_post_ops;
            mov(reg_do_post_ops, ptr[rsp + reg_do_post_ops_offs_]);
            cmp(reg_do_post_ops, 0);
            jz(label_store_without_post_ops, T_NEAR);
            store_accumulators_apply_post_ops(
                    bd_block, ld_block2, 0, is_ld_tail);
            jmp(label_done, T_NEAR);

            L_aligned(label_store_without_post_ops);
        }
        store_accumulators_without_post_ops(bd_block, ld_block2, is_ld_tail);
        L_aligned(label_done);
    }
}

template <typename Wmm>
void jit_brgemm_kernel_t<Wmm>::restore_A_B_matrices() {
    auto restore_reg_batch = brg.brgattr.max_bs > 1 || vpad_exist;
    if (brg.type == brgemm_addr) {
        if (restore_reg_batch) mov(reg_aux1_batch, reg_addr_batch);
    } else {
        mov(reg_aux1_A, reg_A);
        mov(reg_aux1_B, reg_B);

        if (brg.type == brgemm_offs)
            mov(reg_offs_batch, ptr[rsp + origin_offs_batch_offs_]);
        else
            mov(reg_strd_batch, ptr[rsp + origin_strd_batch_offs_]);
    }
}

template <typename Wmm>
void jit_brgemm_kernel_t<Wmm>::set_A_B_matrices() {
    if (brg.type == brgemm_addr) {
        if (brg.brgattr.max_bs > 1) {
            if (brg.layout == brgemm_row_major) {
                mov(reg_aux_A,
                        ptr[reg_aux1_batch + GET_OFF_BATCH_ELEMENT(ptr.A)]);
                mov(reg_aux_B,
                        ptr[reg_aux1_batch + GET_OFF_BATCH_ELEMENT(ptr.B)]);
            } else {
                mov(reg_aux_A,
                        ptr[reg_aux1_batch + GET_OFF_BATCH_ELEMENT(ptr.B)]);
                mov(reg_aux_B,
                        ptr[reg_aux1_batch + GET_OFF_BATCH_ELEMENT(ptr.A)]);
            }
        } else {
            // for max_batch == 1 we stored A and B pointers at the beginning
            // of kernel in reg_aux1_A and reg_aux1_B
            if (brg.layout == brgemm_row_major) {
                mov(reg_aux_A, reg_aux1_A);
                mov(reg_aux_B, reg_aux1_B);
            } else {
                mov(reg_aux_A, reg_aux1_B);
                mov(reg_aux_B, reg_aux1_A);
            }
        }

        if (brg.brgattr.max_bs > 1) {
            add(reg_aux1_batch, sizeof(brgemm_batch_element_t));
            prefetcht0(ptr[reg_aux1_batch]);
        }
    } else if (brg.type == brgemm_offs) {
        mov(reg_aux_A, reg_A);
        mov(reg_aux_B, reg_B);

        add(reg_aux_A, ptr[reg_offs_batch + GET_OFF_BATCH_ELEMENT(offset.A)]);
        add(reg_aux_B, ptr[reg_offs_batch + GET_OFF_BATCH_ELEMENT(offset.B)]);
        add(reg_offs_batch, sizeof(brgemm_batch_element_t));
    } else if (brg.type == brgemm_strd) {
        mov(reg_aux_A, reg_aux1_A);
        mov(reg_aux_B, reg_aux1_B);

        safe_add(reg_aux1_A, brg.stride_a, reg_tmp_gpr);
        safe_add(reg_aux1_B, brg.stride_b, reg_tmp_gpr);
        if (vpad_exist) {
            mov(reg_strd_batch, ptr[rsp + origin_strd_batch_offs_]);
            add(reg_strd_batch, sizeof(brgemm_batch_element_t));
            mov(ptr[rsp + origin_strd_batch_offs_], reg_strd_batch);
        }
    }

    add(reg_aux_A, reg_a_offset);
    add(reg_aux_B, reg_b_offset);
}

template <typename Wmm>
void jit_brgemm_kernel_t<Wmm>::maybe_pre_process_data(matrix_kind_t matrix_kind,
        const Tmm &t1, reg64_t reg_base, size_t offset, reg64_t reg_stride,
        int num_rows, int num_col_bytes, bool is_rd_tail) {
    constexpr int tile_size = 1024;
    const auto transform_offset = brg.brgattr.use_interleave_stores
            ? brg.get_num_C_tiles() * tile_size
            : 0;
    add(reg_buf_aux, transform_offset);

    switch (matrix_kind) {
        case matrix_A:
            fp8_to_f16_upconvert(num_rows, num_col_bytes, reg_base, offset,
                    reg_stride, brg.dt_a, is_rd_tail);
            break;
        case matrix_B:
            fp8_to_f16_upconvert_to_vnni(num_rows, num_col_bytes, reg_base,
                    offset, reg_stride, brg.dt_b, is_rd_tail);
            break;
        default: assert(!"Wrong Matrix");
    }

    // load into tmm from the transformed data.
    mov(reg_converted_stride, zmm_width_in_bytes_);
    tileloadd(t1, ptr[reg_buf_aux + reg_converted_stride]);
}

template <typename Wmm>
void jit_brgemm_kernel_t<Wmm>::maybe_tileloadd_nt(matrix_kind_t matrix_kind,
        int idx, int offset, bool is_rd_tail, bool is_tail) {

    const bool is_A = matrix_kind == matrix_kind_t::matrix_A;

    const int tmm_idx = is_A ? brg.get_A_tensor(idx, is_tail)
                             : brg.get_B_tensor(idx, is_tail);
    auto t1 = Tmm(tmm_idx);

    auto reg_base = is_A ? reg_aux_A : reg_aux_B;

    auto reg_stride = is_A ? reg_stride_lda : reg_stride_ldb;
    bool try_load_nt = brg.innermost_loop
            == (is_A ? brgemm_bd_loop_innermost : brgemm_ld_loop_innermost);

    if (brg.is_fp8_via_convert()) {
        const int typesize_A
                = brg.is_input_convert() ? sizeof(int16_t) : brg.typesize_A;
        const int typesize_B
                = brg.is_input_convert() ? sizeof(int16_t) : brg.typesize_B;
        int rd_step = 4 / typesize_A;
        int rd_block = (!brg.rdb && brg.rdb_tail) ? brg.rdb_tail : brg.rd_block;
        if (brg.is_input_convert()) {
            const int vnni_granularity
                    = data_type_vnni_granularity(data_type::f16);
            rd_block = utils::rnd_up(rd_block, vnni_granularity);
        }

        int A_col = typesize_A * rd_block;
        int A_row = is_tail ? brg.bdb_tail : brg.bd_block;

        int B_col = (is_tail ? brg.ldb_tail : brg.ld_block) * typesize_B
                * rd_step;
        int B_row = brg.typesize_C != 0 ? A_col / brg.typesize_C : 0;
        mov(ptr[rsp + reg_val_tmp_1_], reg64_fp8_aux);
        mov(ptr[rsp + reg_val_tmp_2_], reg_buf_aux);

        mov(reg_buf_aux, ptr[rsp + reg_buf_offs_]);
        maybe_pre_process_data(matrix_kind, t1, reg_base, offset, reg_stride,
                is_A ? A_row : B_row, is_A ? A_col : B_col, is_rd_tail);

        mov(reg64_fp8_aux, ptr[rsp + reg_val_tmp_1_]);
        mov(reg_buf_aux, ptr[rsp + reg_val_tmp_2_]);
    } else {
        const size_t cache_footprint = static_cast<size_t>(brg.typesize_A)
                        * brg.brgattr.hint_expected_A_size
                + static_cast<size_t>(brg.typesize_B)
                        * brg.brgattr.hint_expected_B_size
                + static_cast<size_t>(brg.typesize_C)
                        * brg.brgattr.hint_expected_C_size;
        if (try_load_nt
                && cache_footprint >= platform::get_per_core_cache_size(1))
            tileloaddt1(t1, ptr[reg_base + offset + reg_stride]);
        else
            tileloadd(t1, ptr[reg_base + offset + reg_stride]);
    }
}

template <typename Wmm>
void jit_brgemm_kernel_t<Wmm>::gemm_microkernel_amx(int bd_block2,
        bool is_bdb_tail, int ld_block2, bool is_rd_tail, bool is_ld_tail) {
    auto tdpbxxd = [this](const Tmm &x1, const Tmm &x2, const Tmm &x3) {
        if (brg.is_fp8) {
            if (brg.is_fp8_via_convert())
                tdpfp16ps(x1, x2, x3);
            else
                assert(!"Not supported!");
        } else if (brg.dt_a == data_type::bf16 && brg.dt_b == data_type::bf16) {
            tdpbf16ps(x1, x2, x3);
        } else if (brg.dt_a == data_type::f16 && brg.dt_b == data_type::f16) {
            tdpfp16ps(x1, x2, x3);
        } else if (brg.dt_a == data_type::u8 && brg.dt_b == data_type::u8) {
            tdpbuud(x1, x2, x3);
        } else if (brg.dt_a == data_type::u8 && brg.dt_b == data_type::s8) {
            tdpbusd(x1, x2, x3);
        } else if (brg.dt_a == data_type::s8 && brg.dt_b == data_type::u8) {
            tdpbsud(x1, x2, x3);
        } else if (brg.dt_a == data_type::s8 && brg.dt_b == data_type::s8) {
            tdpbssd(x1, x2, x3);
        } else {
            assert(!"unsupported combination");
        }
    };
    int rbd_block = (is_rd_tail) ? 1 : brg.rdb;
    for (int rdb = 0; rdb < rbd_block; rdb++) {
        for (int bdb = 0; bdb < bd_block2; bdb++) {
            maybe_tileloadd_nt(matrix_kind_t::matrix_A, bdb,
                    rdb * rdb_A_offset() + A_offset(bdb, 0, true), is_rd_tail,
                    is_bdb_tail);
        }
        for (int ldb = 0; ldb < ld_block2; ldb++) {

            const int idx = (is_ld_tail) ? brg.ld_block2 : ldb;
            maybe_tileloadd_nt(matrix_kind_t::matrix_B, idx,
                    rdb * rdb_B_offset() + B_offset(ldb, 0, true), is_rd_tail,
                    is_ld_tail);
            for (int bdb = 0; bdb < bd_block2; bdb++) {
                tdpbxxd(Tmm(brg.get_C_tensor(
                                bdb, idx, is_bdb_tail, is_ld_tail)),
                        Tmm(brg.get_A_tensor(bdb, is_bdb_tail)),
                        Tmm(brg.get_B_tensor(idx, is_ld_tail)));
            }
        }
    }
    if (!is_rd_tail) {
        add(reg_aux_A, brg.rdb * rdb_A_offset());
        add(reg_aux_B, brg.rdb * rdb_B_offset());
    }
}

template <typename Wmm>
void jit_brgemm_kernel_t<Wmm>::dot_product(Vmm v1, Vmm v2, Vmm v3) {
    if (brg.is_f32 || brg.is_f16
            || (brg.is_bf16 && brg.isa_impl == avx2_vnni_2))
        uni_vfmadd231ps(v1, v2, v3);
    else if (brg.is_bf16)
        vdpbf16ps(v1, v2, v3);
    else if (brg.is_int8) {
        if (brg.dt_a == data_type::s8 && isa_has_s8s8(brg.isa_impl))
            vpdpbssd(v1, v3, v2);
        else if (brg.has_int8_vnni)
            vpdpbusd(v1, v3, v2,
                    is_superset(brg.isa_impl, avx512_core) ? EvexEncoding
                                                           : VexEncoding);
        else {
            vpmaddubsw(int8_dot_product_temp(), v3, v2);
            vpmaddwd(int8_dot_product_temp(), int8_dot_product_temp(),
                    int8_ones_words());
            vpaddd(v1, v1, int8_dot_product_temp());
        }
    }
}

template <typename Wmm>
void jit_brgemm_kernel_t<Wmm>::compute_int8_compensation(int rd_loop, int bd_b,
        int bd_e, int bd_block, int ld_block2, bool is_ld_tail, int vpad) {
    assert(brg.is_int8);

    auto compensation_padding = [this, ld_block2](Vmm vmm_load, Vmm vmm_tmp,
                                        int ld, int bd_b, int bd_e) {
        // req_cal_comp_pads -> only calculate compensation along with
        // computation and do not use pre-calculated compensation.
        // Calculate comp padding as:
        // accum - inp_shift * conv(1, wei_s32)
        if (brg.req_s8s8_compensation) {
            if (brg.req_cal_comp_pads) {
                uni_vpxor(vmm_tmp, vmm_tmp, vmm_tmp);
                dot_product(vmm_tmp, vmm_load, vmm_inp_shift());
            }

            for (int bd = bd_b; bd < bd_e; bd++) {
                auto vmm = accm(ld_block2, bd, ld);
                if (brg.req_cal_comp_pads) {
                    uni_vpsubd(vmm, vmm, vmm_tmp);
                } else {
                    dot_product(vmm, vmm_load, vmm_inp_shift());
                }
            }
        }

        if (brg.zp_type_a != brgemm_broadcast_t::none) {
            uni_vpxor(vmm_tmp, vmm_tmp, vmm_tmp);
            dot_product(vmm_tmp, vmm_load, vmm_one_bytes());
            uni_vpmulld(vmm_tmp, vmm_tmp, vmm_zp_a_shift());

            for (int bd = bd_b; bd < bd_e; bd++) {
                auto vmm = accm(ld_block2, bd, ld);
                if (brg.req_cal_comp_pads) {
                    uni_vpsubd(vmm, vmm, vmm_tmp);
                } else {
                    uni_vpaddd(vmm, vmm, vmm_tmp);
                }
            }
        }
    };

    if (n_bcast_1_load && brg.zp_type_a != brgemm_broadcast_t::none) {
        mov(ptr[rsp + reg_bdb_loop_offs_], reg_bdb_loop);
        const auto reg32_scratch = reg_zp_a_input_shift.cvt32();
        mov(reg32_scratch, 0x1010101);
        uni_vpbroadcastd(vmm_one_bytes(), reg32_scratch);
        mov(reg32_scratch, ptr[rsp + reg_zp_a_val_offs_]);
        uni_vpbroadcastd(vmm_zp_a_shift(), reg32_scratch);
        mov(reg_bdb_loop, ptr[rsp + reg_bdb_loop_offs_]);
    }

    for_(int rd = 0; rd < rd_loop; rd += brg.rd_step)
    for (int ld = 0; ld < ld_block2; ++ld) {
        const auto addr = ptr[reg_aux_B + B_offset(ld, rd)];
        const bool is_tail = is_ld_tail && ld + 1 == ld_block2;
        if (IMPLICATION(is_tail, is_superset(brg.isa_impl, avx512_core))) {
            auto vmm_store = vmm_mask(load(), is_tail, false, ld_tail_mask);
            uni_vmovups(vmm_store, addr);
        } else {
            load_bytes(
                    load(), addr, brg.typesize_B * brg.ldb_tail * brg.ld_step);
        }

        if (brg.req_cal_comp_pads) {
            compensation_padding(load(), bcst(), ld, bd_b, bd_e);
        } else if (vpad != 0) {
            if (bd_b > 0) compensation_padding(load(), bcst(), ld, 0, bd_b);
            if (bd_e < bd_block)
                compensation_padding(load(), bcst(), ld, bd_e, bd_block);
        }
    }
}

template <typename Wmm>
void jit_brgemm_kernel_t<Wmm>::gemm_microkernel_dyn_quant(int bd_block2,
        bool is_bdb_tail, int ld_block2, bool is_rd_tail, bool is_ld_tail,
        int vpad, int rows_for_rd_tail) {
    int bd_block = (is_bdb_tail) ? brg.bdb_tail : brg.bd_block;
    const auto bd_b = nstl::max(0, vpad);
    const auto bd_e = nstl::min(bd_block, bd_block + vpad);
    const auto is_valid_bd
            = need_comp_pads && vpad != 0 ? bd_b <= bd_e : bd_b < bd_e;
    if (!is_valid_bd) return;

    bool is_emdbd = brg.embd_bcst;

    int rd_loop = 0, rd_tail_size = 0;
    if (is_rd_tail) {
        if (brg.is_bf16 || brg.is_int8) {
            rd_tail_size = brg.rdb_tail % brg.rd_step;
            rd_loop = (rd_tail_size != 0)
                    ? ((brg.rdb_tail / brg.rd_step) + 1) * brg.rd_step
                    : brg.rdb_tail;
        } else
            rd_loop = brg.rdb_tail;
    } else
        rd_loop = brg.rd_block;

    bool maybe_load_bytes = (rows_for_rd_tail > 0 || brg.brgattr.wary_tail_read)
            && is_rd_tail && rd_tail_size != 0 && (brg.is_bf16 || brg.is_int8);

    auto broadcast = [this, rd_tail_size](Vmm v1, size_t offset, bool is_tail,
                             data_type_t dt) {
        if (is_tail) {
            uni_vpxor(v1, v1, v1);
            Xmm xmm_tmp = Xmm(v1.getIdx());
            load_bytes(
                    xmm_tmp, reg_aux_A, offset, rd_tail_size * brg.typesize_A);
            uni_vpbroadcastd(v1, xmm_tmp);
        } else {
            if (dt == data_type::f32) {
                uni_vbroadcastss(v1, ptr[reg_aux_A + offset]);
            } else if (dt == data_type::bf16) {
                if (brg.isa_impl == avx2_vnni_2)
                    vbcstnebf162ps(v1, ptr[reg_aux_A + offset]);
                else
                    uni_vpbroadcastd(v1, ptr[reg_aux_A + offset]);
            } else if (one_of(dt, data_type::s8, data_type::u8)) {
                uni_vpbroadcastd(v1, ptr[reg_aux_A + offset]);
            } else if (dt == data_type::f16) {
                if (brg.isa_impl == avx2_vnni_2)
                    vbcstnesh2ps(v1, ptr[reg_aux_A + offset]);
                else
                    vcvtph2psx(v1, ptr_b[reg_aux_A + offset]);
            }
        }

        if (brg.req_s8s8_compensation) uni_vpaddb(v1, v1, vmm_inp_shift());
    };

    auto vmm_accm_tmp = [&](int ld_block, int bd, int ld) {
        int idx = max_effective_vregs - 1 - (brg.ld_block2 * brg.bd_block) - ld_block - (bd * ld_block + ld);
        return Vmm(idx);
    };

    auto vmm_zero_point = [&](int ld) {
        int idx = isa_num_vregs(brg.isa_impl) - 3 - ld;
        return Vmm(idx);
    };

    static const int8_t negative_one[64] = {
        -1, -1, -1, -1, -1, -1, -1, -1,
        -1, -1, -1, -1, -1, -1, -1, -1,
        -1, -1, -1, -1, -1, -1, -1, -1,
        -1, -1, -1, -1, -1, -1, -1, -1,
        -1, -1, -1, -1, -1, -1, -1, -1,
        -1, -1, -1, -1, -1, -1, -1, -1,
        -1, -1, -1, -1, -1, -1, -1, -1,
        -1, -1, -1, -1, -1, -1, -1, -1
    };

    static const int8_t mask_low_half[64] = {
        0x0F, 0x0F, 0x0F, 0x0F, 0x0F, 0x0F, 0x0F, 0x0F, 0x0F, 0x0F, 0x0F, 0x0F, 0x0F, 0x0F, 0x0F, 0x0F,
        0x0F, 0x0F, 0x0F, 0x0F, 0x0F, 0x0F, 0x0F, 0x0F, 0x0F, 0x0F, 0x0F, 0x0F, 0x0F, 0x0F, 0x0F, 0x0F,
        0x0F, 0x0F, 0x0F, 0x0F, 0x0F, 0x0F, 0x0F, 0x0F, 0x0F, 0x0F, 0x0F, 0x0F, 0x0F, 0x0F, 0x0F, 0x0F,
        0x0F, 0x0F, 0x0F, 0x0F, 0x0F, 0x0F, 0x0F, 0x0F, 0x0F, 0x0F, 0x0F, 0x0F, 0x0F, 0x0F, 0x0F, 0x0F
    };

    mov(ptr[rsp + reg_bdb_loop_offs_], reg_bdb_loop);
    mov(ptr[rsp + reg_ldb_loop_offs_], reg_ldb_loop);

    auto reg_local_wei_scales = reg_bdb_loop;
    auto reg_local_wei_zp = reg_ldb_loop;
    auto reg_ptr = reg_local_wei_scales;

    if (brg.with_wei_decomp_zero_points) {
        mov(reg_local_wei_zp, ptr[rsp + reg_aux2_wei_zero_points_offs_]);
        if (brg.wei_decomp_zero_points_stride == 0) {
            auto reg_ptr_8 = Reg8(reg_ptr.getIdx());
            mov(reg_ptr_8, ptr[reg_local_wei_zp]);
            uni_vpbroadcastb(vmm_zero_point(0), reg_ptr_8);
        } else {
            static const int8_t index_table[64] = {
                0x00, 0x00, 0x00, 0x00, 0x04, 0x04, 0x04, 0x04, 0x08, 0x08, 0x08, 0x08, 0x0C, 0x0C, 0x0C, 0x0C,
                0x00, 0x00, 0x00, 0x00, 0x04, 0x04, 0x04, 0x04, 0x08, 0x08, 0x08, 0x08, 0x0C, 0x0C, 0x0C, 0x0C,
                0x00, 0x00, 0x00, 0x00, 0x04, 0x04, 0x04, 0x04, 0x08, 0x08, 0x08, 0x08, 0x0C, 0x0C, 0x0C, 0x0C,
                0x00, 0x00, 0x00, 0x00, 0x04, 0x04, 0x04, 0x04, 0x08, 0x08, 0x08, 0x08, 0x0C, 0x0C, 0x0C, 0x0C
            };

            auto vmm_indexes = Vmm(isa_num_vregs(brg.isa_impl) - 1);
            mov(reg_ptr, (size_t)index_table);
            uni_vmovups(vmm_indexes, ptr[reg_ptr]);

            for (int ld = 0; ld < ld_block2; ld++) {
                uni_vpmovzxbd(vmm_zero_point(ld), ptr[reg_local_wei_zp + ld * brg.ld_block * types::data_type_size(brg.wei_decomp_zero_points_dt)]);
                vpshufb(vmm_zero_point(ld), vmm_zero_point(ld), vmm_indexes);
            }
        }
    }

    auto vmm_neg_one = Vmm(isa_num_vregs(brg.isa_impl) - 1);
    mov(reg_ptr, (size_t)negative_one);
    uni_vmovups(vmm_neg_one, ptr[reg_ptr]);

    auto vmm_mask_low_half = Vmm(isa_num_vregs(brg.isa_impl) - 2);
    mov(reg_ptr, (size_t)mask_low_half);
    uni_vmovups(vmm_mask_low_half, ptr[reg_ptr]);

    mov(reg_local_wei_scales, ptr[rsp + reg_aux2_wei_scales_offs_]);

    for (int bd = bd_b; bd < bd_e; bd++) {
        for (int ld = 0; ld < ld_block2; ld++) {
            auto vmm_accm = vmm_accm_tmp(ld_block2, bd, ld);
            uni_vxorps(vmm_accm, vmm_accm, vmm_accm);
        }
    }

    for (int rd = 0; rd < rd_loop; rd += brg.rd_step) {
        int prefetch_count_B = 0;
        for (int ld = 0; ld < ld_block2; ld++) {
            const auto addr = ptr[reg_aux_B + B_offset(ld, rd)];
            const Vmm vmm_load = vmm_mask(load(ld), is_ld_tail, false, ld_tail_mask);
            if (brg.dt_b == data_type::u8) {
                uni_vmovups(vmm_load, addr);
            } else if (brg.dt_b == data_type::u4) {
                uni_vmovups(vmm_load, addr);
                if (rd % 8 == 0)
                    uni_vpsrld(vmm_load, vmm_load, 4);
                uni_vandps(vmm_load, vmm_load, vmm_mask_low_half);
            } else {
                assert(!"unsupported combination");
            }
        }

        bool have_to_load_bytes
                = maybe_load_bytes && (rd == rd_loop - brg.rd_step);

        auto rows_by_load_bytes = have_to_load_bytes ? rows_for_rd_tail : 0;
        for (int bd = bd_b; bd < bd_e; bd++) {
            if (!is_emdbd) {
                const auto bd_by_load_bytes
                        = (bd >= bd_e - rows_by_load_bytes
                                || brg.brgattr.wary_tail_read);
                    broadcast(bcst(), A_offset(bd, rd),
                            have_to_load_bytes && bd_by_load_bytes, brg.dt_a);
            }
            if (prefetch_count_B < ld_block2) {
                prefetcht0(ptr[reg_aux_B + B_offset(prefetch_count_B++, rd)
                        + brg.LDB * brg.rd_block * brg.typesize_B]);
            }
            for (int ld = 0; ld < ld_block2; ld++) {
                auto vmm = vmm_accm_tmp(ld_block2, bd, ld);
                vpdpbusd(vmm, load(ld), bcst(), is_superset(brg.isa_impl, avx512_core) ? EvexEncoding : VexEncoding);
            }
            if (brg.with_wei_decomp_zero_points) {
                uni_vpxor(bcst(), bcst(), vmm_neg_one);
                uni_vpsubb(bcst(), bcst(), vmm_neg_one);
                for (int ld = 0; ld < ld_block2; ld++) {
                    auto vmm =  vmm_accm_tmp(ld_block2, bd, ld);
                    Vmm vmm_zp = brg.wei_decomp_zero_points_stride == 0 ? vmm_zero_point(0) : vmm_zero_point(ld);
                    vpdpbusd(vmm, vmm_zp, bcst(), is_superset(brg.isa_impl, avx512_core) ? EvexEncoding : VexEncoding);
                }
            }
        }
    }

    auto reg_local_src_scales = reg_local_wei_zp;
    auto vmm_src_scales = bcst();
    mov(reg_local_src_scales, ptr[rsp + reg_aux2_src_scales_offs_]);

    for (int bd = bd_b; bd < bd_e; bd++) {
        uni_vbroadcastss(vmm_src_scales, ptr[reg_local_src_scales + bd * brg.src_scales_stride * sizeof(float)]);
        for (int ld = 0; ld < ld_block2; ld++) {
            uni_vmovups(load(ld), ptr[reg_local_wei_scales + ld * brg.ld_block * sizeof(float)]);
        }
        for (int ld = 0; ld < ld_block2; ld++) {
            auto vmm_accm_aux = vmm_accm_tmp(ld_block2, bd, ld);
            auto vmm_accm = accm(ld_block2, bd, ld);

            uni_vcvtdq2ps(vmm_accm_aux, vmm_accm_aux);
            uni_vmulps(vmm_accm_aux, vmm_accm_aux, vmm_src_scales);
            uni_vfmadd231ps(vmm_accm, vmm_accm_aux, load(ld));
        }
    }

    mov(reg_ldb_loop, ptr[rsp + reg_ldb_loop_offs_]);
    mov(reg_bdb_loop, ptr[rsp + reg_bdb_loop_offs_]);

    return;
}

template <typename Wmm>
void jit_brgemm_kernel_t<Wmm>::gemm_microkernel(int bd_block2, bool is_bdb_tail,
        int ld_block2, bool is_rd_tail, bool is_ld_tail, int vpad,
        int rows_for_rd_tail) {
    assert(!brg.is_fp8_via_convert() && "No non-AMX path for fp8");
    MAYBE_UNUSED(bd_block2);

    if (brg.with_src_dyn_quant) {
        gemm_microkernel_dyn_quant(bd_block2, is_bdb_tail, ld_block2, is_rd_tail, is_ld_tail, vpad, rows_for_rd_tail);
        return;
    }

    int bd_block = (is_bdb_tail) ? brg.bdb_tail : brg.bd_block;
    const auto bd_b = nstl::max(0, vpad);
    const auto bd_e = nstl::min(bd_block, bd_block + vpad);
    const auto is_valid_bd
            = need_comp_pads && vpad != 0 ? bd_b <= bd_e : bd_b < bd_e;
    if (!is_valid_bd) return;

    bool is_emdbd = brg.embd_bcst;

    int rd_loop = 0, rd_tail_size = 0;
    if (is_rd_tail) {
        if (brg.is_bf16 || brg.is_int8) {
            rd_tail_size = brg.rdb_tail % brg.rd_step;
            rd_loop = (rd_tail_size != 0)
                    ? ((brg.rdb_tail / brg.rd_step) + 1) * brg.rd_step
                    : brg.rdb_tail;
        } else
            rd_loop = brg.rdb_tail;
    } else
        rd_loop = brg.rd_block;

    auto broadcast = [this, rd_tail_size](Vmm v1, size_t offset, bool is_tail,
                             data_type_t dt) {
        if (is_tail) {
            Xmm xmm_tmp = Xmm(v1.getIdx());
            load_bytes(
                    xmm_tmp, reg_aux_A, offset, rd_tail_size * brg.typesize_A);
            uni_vpbroadcastd(v1, xmm_tmp);
        } else {
            if (dt == data_type::f32) {
                uni_vbroadcastss(v1, ptr[reg_aux_A + offset]);
            } else if (dt == data_type::bf16) {
                if (brg.isa_impl == avx2_vnni_2)
                    vbcstnebf162ps(v1, ptr[reg_aux_A + offset]);
                else
                    uni_vpbroadcastd(v1, ptr[reg_aux_A + offset]);
            } else if (one_of(dt, data_type::s8, data_type::u8)) {
                uni_vpbroadcastd(v1, ptr[reg_aux_A + offset]);
            } else if (dt == data_type::f16) {
                if (brg.isa_impl == avx2_vnni_2)
                    vbcstnesh2ps(v1, ptr[reg_aux_A + offset]);
                else
                    vcvtph2psx(v1, ptr_b[reg_aux_A + offset]);
            }
        }

        if (brg.req_s8s8_compensation) uni_vpaddb(v1, v1, vmm_inp_shift());
    };

    const bool comp_vpad = vpad != 0
            && (brg.req_s8s8_compensation
                    || brg.zp_type_a != brgemm_broadcast_t::none);
    if (brg.req_cal_comp_pads || comp_vpad)
        compute_int8_compensation(
                rd_loop, bd_b, bd_e, bd_block, ld_block2, is_ld_tail, vpad);

    bool maybe_load_bytes = (rows_for_rd_tail > 0 || brg.brgattr.wary_tail_read)
            && is_rd_tail && rd_tail_size != 0 && (brg.is_bf16 || brg.is_int8);
    if (n_bcast_1_load) {
        for (int rd = 0; rd < rd_loop; rd += brg.rd_step) {
            bool have_to_load_bytes
                    = maybe_load_bytes && (rd == rd_loop - brg.rd_step);

            auto rows_by_load_bytes = have_to_load_bytes ? rows_for_rd_tail : 0;
            for (int bd = bd_b; bd < bd_e && !is_emdbd; bd++) {
                const auto bd_by_load_bytes = (bd >= bd_e - rows_by_load_bytes
                        || brg.brgattr.wary_tail_read);
                broadcast(bcst(bd), A_offset(bd, rd),
                        have_to_load_bytes && bd_by_load_bytes, brg.dt_a);
            }
            for (int ld = 0; ld < ld_block2; ld++) {
                const auto addr = ptr[reg_aux_B + B_offset(ld, rd)];
                const Vmm vmm_load
                        = vmm_mask(load(), is_ld_tail, false, ld_tail_mask);
                // Note: Assuming the tails are properly padded/blocked for
                // avx2_vnni_2 with xf16 data type, as the B matrix is generally
                // at least double-blocked.
                if (brg.dt_b == data_type::f16) {
                    if (brg.isa_impl == avx2_vnni_2) {
                        if (rd % 2 == 0)
                            vcvtneeph2ps(vmm_load, addr);
                        else
                            vcvtneoph2ps(vmm_load, addr);
                    } else if (brg.isa_impl == avx512_core_fp16) {
                        vcvtph2psx(vmm_load, addr);
                    } else {
                        vcvtph2ps(vmm_load, addr);
                    }
                } else if (brg.dt_b == data_type::bf16) {
                    if (brg.isa_impl == avx2_vnni_2) {
                        if (rd % 2 == 0)
                            vcvtneebf162ps(vmm_load, addr);
                        else
                            vcvtneobf162ps(vmm_load, addr);
                    } else {
                        vpmovzxwd(vmm_load, addr);
                        uni_vpslld(vmm_load, vmm_load, 16);
                    }
                } else if (is_ld_tail) {
                    if (is_superset(brg.isa_impl, avx512_core)) {
                        uni_vmovups(vmm_load, addr);
                    } else {
                        load_bytes(vmm_load, addr,
                                brg.typesize_B * brg.ldb_tail * brg.ld_step);
                    }
                } else {
                    uni_vmovups(vmm_load, addr);
                }
                for (int bd = bd_b; bd < bd_e; bd++) {
                    auto vmm = accm(ld_block2, bd, ld);
                    if (is_emdbd)
                        uni_vfmadd231ps(vmm, load(),
                                ptr_b[reg_aux_A + A_offset(bd, rd)]);
                    else
                        dot_product(vmm, load(), bcst(bd));
                }
            }
        }
    } else {
        if (brg.with_wei_decomp) {
            auto reg_local_wei_scales = reg_bdb_loop;
            auto reg_local_wei_zp = reg_ldb_loop;
            auto reg_ptr = reg_local_wei_zp;

            auto accm_tmp = [&](int ld_block, int bd, int ld) {
                int idx = max_effective_vregs - 1 - 2 * (brg.ld_block2 * brg.bd_block) - ld;
                return Vmm(idx);
            };

            auto load_zero_points = [&](Vmm vmm_zp, Xbyak::Address addr) {
                if (brg.wei_decomp_zero_points_stride == 0) {
                    switch (brg.wei_decomp_zero_points_dt) {
                        case data_type::f32: {
                            uni_vbroadcastss(vmm_zp, addr);
                            break;
                        }
                        case data_type::u8: {
                            auto xmm_zp = Xmm(vmm_zp.getIdx());
                            auto reg_ptr_32 = Reg32(reg_ptr.getIdx());
                            movzx(reg_ptr_32, addr);
                            uni_vmovq(xmm_zp, reg_ptr);
                            uni_vcvtdq2ps(xmm_zp, xmm_zp);
                            uni_vbroadcastss(vmm_zp, xmm_zp);
                            break;
                        }
                        default: assert(!"unsupported data type");
                    }
                } else {
                    switch (brg.wei_decomp_zero_points_dt) {
                        case data_type::f32: {
                            uni_vmovups(vmm_zp, addr);
                            break;
                        }
                        case data_type::u8: {
                            uni_vpmovzxbd(vmm_zp, addr);
                            uni_vcvtdq2ps(vmm_zp, vmm_zp);
                            break;
                        }
                        default: assert(!"unsupported data type");
                    }
                }
            };

            mov(ptr[rsp + reg_bdb_loop_offs_], reg_bdb_loop);
            mov(ptr[rsp + reg_ldb_loop_offs_], reg_ldb_loop);

            auto vmm_zero_points = Vmm(isa_num_vregs(brg.isa_impl) - 1);
            auto vmm_mask8 = Vmm(isa_num_vregs(brg.isa_impl) - 1);
            auto vmm_mask7 = Vmm(isa_num_vregs(brg.isa_impl) - 2);
            auto vmm_lookup = Vmm(isa_num_vregs(brg.isa_impl) - 1);
            auto vmm_lookup_low = Vmm(isa_num_vregs(brg.isa_impl) - 3);
            auto vmm_lookup_high = Vmm(isa_num_vregs(brg.isa_impl) - 4);
            if (brg.dt_b == data_type::nf4) {
                static const float lookup[16] = {
                    -1.0,
                    -0.6961928009986877,
                    -0.5250730514526367,
                    -0.39491748809814453,
                    -0.28444138169288635,
                    -0.18477343022823334,
                    -0.09105003625154495,
                    0.0,
                    0.07958029955625534,
                    0.16093020141124725,
                    0.24611230194568634,
                    0.33791524171829224,
                    0.44070982933044434,
                    0.5626170039176941,
                    0.7229568362236023,
                    1.0};

                static const int32_t mask8[16] = {
                    8, 8, 8, 8, 8, 8, 8, 8,
                    8, 8, 8, 8, 8, 8, 8, 8
                };
                static const int32_t mask7[16] = {
                    7, 7, 7, 7, 7, 7, 7, 7,
                    7, 7, 7, 7, 7, 7, 7, 7
                };

                if (brg.isa_impl == avx2) {
                    mov(reg_ptr, (size_t)lookup);
                    uni_vmovups(vmm_lookup_low, ptr[reg_ptr]);
                    mov(reg_ptr, (size_t)lookup);
                    uni_vmovups(vmm_lookup_high, ptr[reg_ptr + 8 * sizeof(float)]);
                    mov(reg_ptr, (size_t)mask8);
                    uni_vmovups(vmm_mask8, ptr[reg_ptr]);
                    mov(reg_ptr, (size_t)mask7);
                    uni_vmovups(vmm_mask7, ptr[reg_ptr]);
                    if (brg.wei_decomp_zero_points_stride == 0)
                        vmm_zero_points = Vmm(isa_num_vregs(brg.isa_impl) - 6);
                    else
                        vmm_zero_points = Vmm(isa_num_vregs(brg.isa_impl) - 5);
                } else {
                    mov(reg_ptr, (size_t)lookup);
                    uni_vmovups(vmm_lookup, ptr[reg_ptr]);
                    vmm_zero_points = Vmm(isa_num_vregs(brg.isa_impl) - 2);
                }
            }

            mov(reg_local_wei_scales, ptr[rsp + reg_aux2_wei_scales_offs_]);
            mov(reg_local_wei_zp, ptr[rsp + reg_aux2_wei_zero_points_offs_]);

            if (brg.with_wei_decomp_zero_points && brg.wei_decomp_zero_points_stride == 0) {
                load_zero_points(vmm_zero_points, ptr[reg_local_wei_zp]);
            }

            for (int rd = 0; rd < rd_loop; rd += brg.rd_step) {
                int prefetch_count_B = 0;
                for (int ld = 0; ld < ld_block2; ld++) {
                    const auto addr = ptr[reg_aux_B + B_offset(ld, rd)];
                    const Vmm vmm_load = vmm_mask(load(ld), is_ld_tail, false, ld_tail_mask);
                    if (brg.dt_b == data_type::u8) {
                        uni_vpmovzxbd(vmm_load, addr);
                        uni_vcvtdq2ps(vmm_load, vmm_load);
                    } else if (brg.dt_b == data_type::s8) {
                        uni_vpmovsxbd(vmm_load, addr);
                        uni_vcvtdq2ps(vmm_load, vmm_load);
                    } else if (brg.dt_b == data_type::u4) {
                        uni_vpmovzxbd(vmm_load, addr);
                        if (rd % 2 == 0) {
                            uni_vpsrld(vmm_load, vmm_load, 4);
                        } else {
                            uni_vpslld(vmm_load, vmm_load, 28);
                            uni_vpsrld(vmm_load, vmm_load, 28);
                        }
                        uni_vcvtdq2ps(vmm_load, vmm_load);
                    } else if (brg.dt_b == data_type::s4) {
                        if (rd % 2 == 0) {
                            uni_vpmovsxbd(vmm_load, addr);
                            vpsrad(vmm_load, vmm_load, 4);
                        } else {
                            uni_vpmovsxbd(vmm_load, addr);
                            uni_vpslld(vmm_load, vmm_load, 28);
                            vpsrad(vmm_load, vmm_load, 28);
                        }
                        uni_vcvtdq2ps(vmm_load, vmm_load);
                    } else if (brg.dt_b == data_type::nf4) {
                        uni_vpmovzxbd(vmm_load, addr);
                        if (rd % 2 == 0) {
                            uni_vpsrld(vmm_load, vmm_load, 4);
                        } else {
                            uni_vpslld(vmm_load, vmm_load, 28);
                            uni_vpsrld(vmm_load, vmm_load, 28);
                        }

                        if (brg.isa_impl == avx2) {
                            auto res = bcst();
                            auto mask = Vmm(isa_num_vregs(brg.isa_impl) - 5);
                            vpcmpgtd(mask, vmm_load, vmm_mask7);
                            vpermd(res, vmm_load, vmm_lookup_low);
                            vpsubd(vmm_load, vmm_load, vmm_mask8);
                            vpermd(vmm_load, vmm_load, vmm_lookup_high);
                            vblendvps(vmm_load, res, vmm_load, mask);
                        } else {
                            vpermd(vmm_load, vmm_load, vmm_lookup);
                        }
                    } else {
                        assert(!"unsupported combination");
                    }

                    if (brg.with_wei_decomp_zero_points) {
                        if (brg.wei_decomp_zero_points_stride == 0) {
                            uni_vsubps(vmm_load, vmm_load, vmm_zero_points);
                        } else {
                            load_zero_points(bcst(), ptr[reg_local_wei_zp + ld * brg.ld_block * types::data_type_size(brg.wei_decomp_zero_points_dt)]);
                            uni_vsubps(vmm_load, vmm_load, bcst());
                        }
                    }

                    if (brg.with_wei_decomp_scales && brg.bd_block != 1) {
                        if (brg.wei_decomp_scales_stride == 0) {
                            uni_vbroadcastss(bcst(), ptr[reg_local_wei_scales]);
                        } else {
                            uni_vmovups(bcst(), ptr[reg_local_wei_scales + ld * brg.ld_block * sizeof(float)]);
                        }
                        uni_vmulps(vmm_load, vmm_load, bcst());
                    }
                }

                bool have_to_load_bytes
                        = maybe_load_bytes && (rd == rd_loop - brg.rd_step);

                auto rows_by_load_bytes = have_to_load_bytes ? rows_for_rd_tail : 0;
                for (int bd = bd_b; bd < bd_e; bd++) {
                    if (!is_emdbd) {
                        const auto bd_by_load_bytes
                                = (bd >= bd_e - rows_by_load_bytes
                                        || brg.brgattr.wary_tail_read);
                        if (brg.dt_a == data_type::bf16) {
                            vpbroadcastw(bcst(), ptr[reg_aux_A + A_offset(bd, rd)]);
                            uni_vpmovzxwd(bcst(), bcst());
                            uni_vpslld(bcst(), bcst(), 16);
                        } else {
                            broadcast(bcst(), A_offset(bd, rd),
                                    have_to_load_bytes && bd_by_load_bytes, brg.dt_a);
                        }
                    }
                    if (prefetch_count_B < ld_block2) {
                        prefetcht0(ptr[reg_aux_B + B_offset(prefetch_count_B++, rd)
                                + brg.LDB * brg.rd_block * brg.typesize_B]);
                    }
                    for (int ld = 0; ld < ld_block2; ld++) {
                        auto vmm = brg.bd_block != 1 ? accm(ld_block2, bd, ld)
                                                     : accm_tmp(ld_block2, bd, ld);
                        if (brg.bd_block == 1 && rd == 0) {
                            if (is_emdbd)
                                uni_vmulps(vmm, load(ld), ptr_b[reg_aux_A + A_offset(bd, rd)]);
                            else
                                uni_vmulps(vmm, load(ld), bcst());
                        } else {
                            if (is_emdbd)
                                uni_vfmadd231ps(vmm, load(ld), ptr_b[reg_aux_A + A_offset(bd, rd)]);
                            else
                                uni_vfmadd231ps(vmm, load(ld), bcst());
                        }
                    }
                }
            }

            if (brg.with_wei_decomp_scales && brg.bd_block == 1) {
                for (int ld = 0; ld < ld_block2; ld++) {
                    auto vmm_accm_tmp = accm_tmp(ld_block2, 0, ld);
                    auto vmm_accm = accm(ld_block2, 0, ld);
                    uni_vmovups(bcst(), ptr[reg_local_wei_scales + ld * brg.ld_block * sizeof(float)]);
                    uni_vfmadd231ps(vmm_accm, vmm_accm_tmp, bcst());
                }
            }

            mov(reg_ldb_loop, ptr[rsp + reg_ldb_loop_offs_]);
            mov(reg_bdb_loop, ptr[rsp + reg_bdb_loop_offs_]);

            return;
        }

        for (int rd = 0; rd < rd_loop; rd += brg.rd_step) {
            int prefetch_count_B = 0;
            for (int ld = 0; ld < ld_block2; ld++) {
                const auto addr = ptr[reg_aux_B + B_offset(ld, rd)];
                const Vmm vmm_load
                        = vmm_mask(load(ld), is_ld_tail, false, ld_tail_mask);
                // Note: Assuming the tails are properly padded/blocked for
                // avx2_vnni_2, as the B matrix is generally
                // at least double-blocked.
                if (brg.dt_b == data_type::f16) {
                    if (brg.isa_impl == avx2_vnni_2) {
                        if (rd % 2 == 0)
                            vcvtneeph2ps(vmm_load, addr);
                        else
                            vcvtneoph2ps(vmm_load, addr);
                    }  if (brg.isa_impl == avx512_core_fp16) {
                        vcvtph2psx(vmm_load, addr);
                    } else {
                        vcvtph2ps(vmm_load, addr);
                    }
                } else if (brg.dt_b == data_type::bf16) {
                    if (brg.isa_impl == avx2_vnni_2) {
                        if (rd % 2 == 0)
                            vcvtneebf162ps(vmm_load, addr);
                        else
                            vcvtneobf162ps(vmm_load, addr);
                    } else {
                        vpmovzxwd(vmm_load, addr);
                        uni_vpslld(vmm_load, vmm_load, 16);
                    }
                } else if (is_ld_tail) {
                    if (is_superset(brg.isa_impl, avx512_core)) {
                        uni_vmovups(vmm_load, addr);
                    } else {
                        load_bytes(vmm_load, addr,
                                brg.typesize_B * brg.ldb_tail * brg.ld_step);
                    }
                } else {
                    uni_vmovups(vmm_load, addr);
                }
            }

            bool have_to_load_bytes
                    = maybe_load_bytes && (rd == rd_loop - brg.rd_step);

            auto rows_by_load_bytes = have_to_load_bytes ? rows_for_rd_tail : 0;
            for (int bd = bd_b; bd < bd_e; bd++) {
                if (!is_emdbd) {
                    const auto bd_by_load_bytes
                            = (bd >= bd_e - rows_by_load_bytes
                                    || brg.brgattr.wary_tail_read);
                    broadcast(bcst(), A_offset(bd, rd),
                            have_to_load_bytes && bd_by_load_bytes, brg.dt_a);
                }
                if (prefetch_count_B < ld_block2) {
                    prefetcht0(ptr[reg_aux_B + B_offset(prefetch_count_B++, rd)
                            + brg.LDB * brg.rd_block * brg.typesize_B]);
                }
                for (int ld = 0; ld < ld_block2; ld++) {
                    auto vmm = accm(ld_block2, bd, ld);
                    if (is_emdbd)
                        uni_vfmadd231ps(vmm, load(ld),
                                ptr_b[reg_aux_A + A_offset(bd, rd)]);
                    else
                        dot_product(vmm, load(ld), bcst());
                }
            }
        }
    }
}

template <typename Wmm>
void jit_brgemm_kernel_t<Wmm>::ldb_loop(int bd_block2, bool is_bdb_tail,
        int ld_block2, int ldb_loop_length, bool is_reg_tail, bool is_ld_tail,
        bool check_top_vpad, bool check_bottom_vpad, int rows_for_rd_tail,
        bool skip_accumulation) {

    Label ldb_loop_label;
    Label BS_loop_label;

    copy_post_ops_stack_values_to_aux(is_reg_tail);

    auto ld_loop_body = [&](int vpad) {
        set_A_B_matrices();

        int bd_block = (is_bdb_tail) ? brg.bdb_tail : brg.bd_block;
        const auto bd_b = nstl::max(0, vpad);
        const auto bd_e = nstl::min(bd_block, bd_block + vpad);
        const auto is_valid_bd
                = need_comp_pads && vpad != 0 ? bd_b <= bd_e : bd_b < bd_e;
        if (!is_valid_bd) return;

        if (brg.is_tmm) {
            const bool is_rd_tail = false;
            gemm_microkernel_amx(
                    bd_block2, is_bdb_tail, ld_block2, is_rd_tail, is_ld_tail);
        } else {
            if (brg.rdb > 0) {
                Label rdb_loop_label;
                mov(reg_rdb_loop, brg.rdb);
                L_aligned(rdb_loop_label, 64);
                {
                    if (brg.with_grouped_wei_decomp && (brg.wei_decomp_scales_stride != 0 ||
                                                        brg.wei_decomp_zero_points_stride != 0)) {
                        auto reg_local_ic = reg_aux_D;
                        auto reg_local_wei_params = reg_bdb_loop;
                        auto reg_local_ic_group = reg_ldb_loop;

                        auto ic_group_shift = [&](int src_offs, int dst_offs, int group_size, int stride) {
                            mov(reg_local_ic, ptr[rsp + reg_aux_ic_offs_]);
                            mov(reg_local_ic_group, group_size);
                            xor_(rdx, rdx);
                            idiv(reg_local_ic_group);
                            imul(reg_local_ic, reg_local_ic, stride);

                            mov(reg_local_wei_params, ptr[rsp + src_offs]);
                            add(reg_local_wei_params, reg_local_ic);
                            mov(ptr[rsp + dst_offs], reg_local_wei_params);
                        };

                        mov(ptr[rsp + reg_bdb_loop_offs_], reg_bdb_loop);
                        mov(ptr[rsp + reg_aux2_D_offs_], reg_aux_D);
                        mov(ptr[rsp + reg_ldb_loop_offs_], reg_ldb_loop);
                        mov(ptr[rsp + reg_reg_a_offset_offs_], reg_a_offset); // preserve rdx for idiv

                        if (brg.with_wei_decomp_scales && brg.wei_decomp_scales_stride != 0) {
                            ic_group_shift(reg_aux_wei_scales_offs_, reg_aux2_wei_scales_offs_,
                                           brg.wei_decomp_scales_group_size, brg.wei_decomp_scales_stride * sizeof(float));
                        }

                        if (brg.with_wei_decomp_zero_points && brg.wei_decomp_zero_points_stride != 0) {
                            ic_group_shift(reg_aux_wei_zero_points_offs_, reg_aux2_wei_zero_points_offs_,
                                           brg.wei_decomp_zero_points_group_size, brg.wei_decomp_zero_points_stride * types::data_type_size(brg.wei_decomp_zero_points_dt));
                        }

                        if (brg.with_src_dyn_quant) {
                            ic_group_shift(reg_aux_src_scales_offs_, reg_aux2_src_scales_offs_,
                                           brg.src_scales_group_size, sizeof(float));
                        }

                        mov(reg_local_ic, ptr[rsp + reg_aux_ic_offs_]);
                        add(reg_local_ic, brg.rd_block);
                        mov(ptr[rsp + reg_aux_ic_offs_], reg_local_ic);

                        mov(reg_bdb_loop, ptr[rsp + reg_bdb_loop_offs_]);
                        mov(reg_aux_D, ptr[rsp + reg_aux2_D_offs_]);
                        mov(reg_ldb_loop, ptr[rsp + reg_ldb_loop_offs_]);
                        mov(reg_a_offset, ptr[rsp + reg_reg_a_offset_offs_]);
                    }

                    const bool is_rd_tail = false;
                    gemm_microkernel(bd_block2, is_bdb_tail, ld_block2,
                            is_rd_tail, is_ld_tail, vpad, rows_for_rd_tail);

                    add(reg_aux_A, rdb_A_offset());
                    add(reg_aux_B, rdb_B_offset());

                    dec(reg_rdb_loop);
                    cmp(reg_rdb_loop, 0);
                }
                jg(rdb_loop_label, T_NEAR);
            }
        }
        if (brg.rdb_tail != 0) {
            const bool is_rd_tail = true;
            if (brg.is_tmm) {
                gemm_microkernel_amx(bd_block2, is_bdb_tail, ld_block2,
                        is_rd_tail, is_ld_tail);
            } else {
                gemm_microkernel(bd_block2, is_bdb_tail, ld_block2, is_rd_tail,
                        is_ld_tail, vpad, rows_for_rd_tail);
            }
        }
    };
    if (is_ldb_loop_) {
        mov(reg_ldb_loop, ldb_loop_length);
        if (brg.is_tmm) mov(ptr[rsp + reg_ldb_loop_offs_], reg_ldb_loop);
    }

    L_aligned(ldb_loop_label, 64);
    {
        zero_accumulators(bd_block2, is_bdb_tail, ld_block2, is_ld_tail,
                skip_accumulation);

        if (is_ldb_loop_)
            mov(ptr[rsp + reg_D_offs_], reg_D);
        else {
            mov(reg_ldb_loop, reg_D);
            if (brg.is_tmm) mov(ptr[rsp + reg_ldb_loop_offs_], reg_ldb_loop);
        }
        if (brg.brgattr.max_bs > 1) mov(ptr[rsp + reg_aux_D_offs_], reg_aux_D);

        if (brg.alpha != 0.f && !skip_accumulation) {
            restore_A_B_matrices();
            if (brg.is_tmm) {
                mov(reg_stride_lda, brg.typesize_A * brg.LDA);
                mov(reg_stride_ldb, brg.rd_step * brg.typesize_B * brg.LDB);
            }

            if (brg.req_s8s8_compensation) {
                mov(ptr[rsp + reg_bdb_loop_offs_], reg_bdb_loop);
                mov(reg_s8_input_shift, 128);
                uni_vpbroadcastb(vmm_inp_shift(), reg_s8_input_shift.cvt8());
                mov(reg_bdb_loop, ptr[rsp + reg_bdb_loop_offs_]);
            }
            if (need_comp_pads && brg.zp_type_a != brgemm_broadcast_t::none) {
                mov(ptr[rsp + reg_bdb_loop_offs_], reg_bdb_loop);
                const auto reg32_scratch = reg_zp_a_input_shift.cvt32();
                mov(reg32_scratch, 0x1010101);
                uni_vpbroadcastd(vmm_one_bytes(), reg32_scratch);
                mov(reg32_scratch, ptr[rsp + reg_zp_a_val_offs_]);
                uni_vpbroadcastd(vmm_zp_a_shift(), reg32_scratch);
                mov(reg_bdb_loop, ptr[rsp + reg_bdb_loop_offs_]);
            }

            if (brg.brgattr.max_bs > 1) mov(reg_BS_loop, reg_BS);
            L_aligned(BS_loop_label, 64);
            {
                if (check_top_vpad || check_bottom_vpad) {
                    const auto vpad_first = -brg.brgattr.max_bottom_vpad;
                    const auto vpad_last = brg.brgattr.max_top_vpad;
                    const auto n_vpads = vpad_last - vpad_first + 2;
                    constexpr auto MAX_N_VPADS = 2 * brgemm_desc_t::MAX_VPAD;
                    assert(n_vpads < MAX_N_VPADS);

                    Label Vpad_loop_end_label;
                    std::vector<Label> Vpad_loop_iter_label(MAX_N_VPADS);
                    if (vpad_exist) {
                        reg64_t reg_batch = (brg.type == brgemm_addr)
                                ? reg_aux1_batch
                                : ((brg.type == brgemm_offs) ? reg_offs_batch
                                                             : reg_strd_batch);
                        if (brg.type == brgemm_strd)
                            mov(reg_strd_batch,
                                    ptr[rsp + origin_strd_batch_offs_]);

                        mov(reg_aux_A_vpad,
                                ptr[reg_batch
                                        + GET_OFF_BATCH_ELEMENT(vvpad.top)]);
                        sub(reg_aux_A_vpad,
                                ptr[reg_batch
                                        + GET_OFF_BATCH_ELEMENT(vvpad.bottom)]);
                    } else
                        xor_(reg_aux_A_vpad, reg_aux_A_vpad);

                    for (int vpad = vpad_first; vpad <= vpad_last; vpad++) {
                        const auto label_vpad = vpad - vpad_first;
                        L(Vpad_loop_iter_label[label_vpad]);
                        if (!check_top_vpad && vpad > 0) continue;
                        if (!check_bottom_vpad && vpad < 0) continue;
                        auto real_vpad = vpad;
                        if (check_bottom_vpad && brg.bdb_tail && vpad < 0) {
                            if (!is_bdb_tail) {
                                // for last full block before
                                // bdb_tail && -vpad greater than bdb_tail
                                if (brg.bdb_tail < -vpad)
                                    real_vpad += brg.bdb_tail;
                                else
                                    continue;
                            } else {
                                // for block with tail, call ldb_loop()
                                // to only calculate compensation for
                                // padding area when bdb_tail < -vpad for
                                // the cases using pre-cal compensation
                                if (brg.bdb_tail < -vpad && need_comp_pads
                                        && !brg.req_cal_comp_pads)
                                    real_vpad = -brg.bdb_tail;
                            }
                        }
                        cmp(reg_aux_A_vpad, vpad);
                        jne(Vpad_loop_iter_label[label_vpad + 1], T_NEAR);
                        ld_loop_body(real_vpad);
                        jmp(Vpad_loop_end_label, T_NEAR);
                    }
                    L(Vpad_loop_iter_label[n_vpads - 1]);
                    ld_loop_body(0);
                    L(Vpad_loop_end_label);
                } else {
                    ld_loop_body(0);
                }
                if (brg.brgattr.max_bs > 1) {
                    dec(reg_BS_loop);
                    cmp(reg_BS_loop, 0);
                    jg(BS_loop_label, T_NEAR);
                }
            }
        }

        if (is_ldb_loop_)
            mov(reg_D, ptr[rsp + reg_D_offs_]);
        else {
            if (brg.is_tmm) mov(reg_ldb_loop, ptr[rsp + reg_ldb_loop_offs_]);
            mov(reg_D, reg_ldb_loop);
        }
        if (brg.brgattr.max_bs > 1) mov(reg_aux_D, ptr[rsp + reg_aux_D_offs_]);

        store_accumulators(bd_block2, is_bdb_tail, ld_block2, is_ld_tail,
                skip_accumulation);
        if (is_ldb_loop_) {
            if (brg.is_tmm) mov(reg_ldb_loop, ptr[rsp + reg_ldb_loop_offs_]);
            if (!is_ld_tail)
                ldb_regs_shift(ld_block2);
            else
                ldb_regs_shift(1, true);
            dec(reg_ldb_loop);
            cmp(reg_ldb_loop, 0);
            if (brg.is_tmm) mov(ptr[rsp + reg_ldb_loop_offs_], reg_ldb_loop);
            jg(ldb_loop_label, T_NEAR);
        }
    }
}

template <typename Wmm>
void jit_brgemm_kernel_t<Wmm>::bdb_loop() {
    auto do_ldb_loop = [this](int bd_block2, bool is_bdb_tail,
                               bool check_top_vpad, bool check_bottom_vpad,
                               int rows_for_rd_tail, bool skip_accumulation) {
        if (brg.ldb2 > 0) {
            const bool is_ld_reg_tail = false;
            const bool is_ld_tail = false;
            ldb_loop(bd_block2, is_bdb_tail, brg.ld_block2, brg.ldb2,
                    is_ld_reg_tail, is_ld_tail, check_top_vpad,
                    check_bottom_vpad, rows_for_rd_tail, skip_accumulation);
        }
        if (brg.ldb2_tail > 0) {
            const bool is_ld_reg_tail = (brg.ldb2 == 0) ? false : true;
            const bool is_ld_tail = false;
            ldb_loop(bd_block2, is_bdb_tail, brg.ldb2_tail, 1, is_ld_reg_tail,
                    is_ld_tail, check_top_vpad, check_bottom_vpad,
                    rows_for_rd_tail, skip_accumulation);
        }
        if (brg.ldb_tail > 0) {
            const bool is_ld_reg_tail
                    = (brg.ldb2 == 0 && brg.ldb2_tail == 0) ? false : true;
            const bool is_ld_tail = true;
            ldb_loop(bd_block2, is_bdb_tail, 1, 1, is_ld_reg_tail, is_ld_tail,
                    check_top_vpad, check_bottom_vpad, rows_for_rd_tail,
                    skip_accumulation);
        }
    };

    auto bdb_loop_body = [this, do_ldb_loop](int bd_block2, bool is_bdb_tail,
                                 bool check_top_vpad, bool check_bottom_vpad,
                                 int rows_for_rd_tail, bool skip_accumulation) {
        do_ldb_loop(bd_block2, is_bdb_tail, check_top_vpad, check_bottom_vpad,
                rows_for_rd_tail, skip_accumulation);

        if (brg.is_runtime_ldc) {
            mov(ptr[rsp + reg_aux_C_bdb_loop_backup_offs_], reg_C);
            xor_(reg_C, reg_C);
            imul(reg_C, ptr[rsp + reg_C_shift_bytes_offs_],
                    bdb_C_offset(bd_block2));
            add(reg_C, ptr[rsp + reg_aux_C_bdb_loop_backup_offs_]);
        } else {
            add(reg_C, bdb_C_offset(bd_block2));
        }
        if (brg.is_runtime_ldd) {
            mov(ptr[rsp + reg_aux_D_bdb_loop_backup_offs_], reg_D);
            xor_(reg_D, reg_D);
            imul(reg_D, ptr[rsp + reg_D_shift_bytes_offs_],
                    bdb_D_offset(bd_block2));
            add(reg_D, ptr[rsp + reg_aux_D_bdb_loop_backup_offs_]);
        } else {
            add(reg_D, bdb_D_offset(bd_block2));
        }
        add(reg_a_offset, bdb_A_offset(bd_block2));

        if (brg.with_src_dyn_quant) {
            mov(reg_src_scales, ptr[rsp + reg_src_scales_offs_]);
            add(reg_src_scales, bd_block2 * brg.bd_block * brg.src_scales_stride * sizeof(float));
            mov(ptr[rsp + reg_src_scales_offs_], reg_src_scales);
        }

        advance_bd_block2_post_op_regs(bd_block2);
    };

    int rows_for_rd_tail, bd_blocks_for_rd_tail;

    if (brg.is_tmm) {
        rows_for_rd_tail = 0;
        bd_blocks_for_rd_tail = 0;
        n_bcast_1_load = false;
    } else {
        rows_for_rd_tail = 0;
        if (brg.rdb_tail != 0 && (brg.is_bf16 || brg.is_int8)) {
            const auto rd_tail_size = brg.rdb_tail % brg.rd_step;
            rows_for_rd_tail = rd_tail_size
                    ? div_up(brg.rd_step - rd_tail_size, brg.reduce_dim)
                    : 0;
        }
        bd_blocks_for_rd_tail
                = div_up(nstl::max(0,
                                 rows_for_rd_tail - brg.bdb_tail
                                         + brg.brgattr.max_bottom_vpad),
                        brg.bd_block);

        auto ld_block2 = (brg.ldb2 > 0)
                ? brg.ld_block2
                : ((brg.ldb2_tail > 0) ? brg.ldb2_tail : 1);
        const int free_vregs = max_effective_vregs - brg.req_s8s8_compensation;
        n_bcast_1_load = brg.is_int8
                && ((brg.bd_block * (ld_block2 + 1) < free_vregs)
                        && (bd_blocks_for_rd_tail == 0)
                        && (rows_for_rd_tail == 0))
                && !brg.with_src_dyn_quant;
        if (brg.brgattr.hint_loop_order != brgemm_lo_default)
            n_bcast_1_load = (brg.brgattr.hint_loop_order == brgemm_lo_bl_1load)
                    ? true
                    : false;
    }

    auto bdb_loop_avx512 = [&](bool skip_accumulation) {
        Label bdb_loop_end_label, no_vpad_label;
        if (vpad_exist) {
            // max_top_vp is restricted by bd_block due to
            // brgemm_kernel implementation. TODO: remove this restriction
            assert(brg.brgattr.max_top_vpad <= brg.bd_block
                    && brg.brgattr.max_bottom_vpad <= brg.bd_block);

            if (brg.type == brgemm_strd) {
                // if batch is nullptr then it means no vpadding in this call
                cmp(reg_offs_batch, 0);
                je(no_vpad_label, T_NEAR);
            }

            // first bd_block --------------
            auto bdblocks = brg.bdb;
            if (bdblocks >= 1) {
                bdb_loop_body(1, false, true,
                        (brg.bcast_dim - brg.brgattr.max_bottom_vpad)
                                < brg.bd_block,
                        brg.bdb - bd_blocks_for_rd_tail > 0 ? 0
                                                            : rows_for_rd_tail,
                        skip_accumulation);
                bdblocks--;
            }
            if (bdblocks > 1) {
                // middle bd_blocks -----------
                Label bdb_loop_label;
                mov(reg_bdb_loop, bdblocks);
                L_aligned(bdb_loop_label, 64);
                {
                    bdb_loop_body(1, false, false, false,
                            bd_blocks_for_rd_tail <= 1 ? 0 : rows_for_rd_tail,
                            skip_accumulation);
                    dec(reg_bdb_loop);
                    cmp(reg_bdb_loop, 1);
                    jg(bdb_loop_label, T_NEAR);
                }
                bdblocks = 1;
            }
            if (bdblocks == 1) {
                // last bd_block ------------
                bdb_loop_body(1, false, false, true,
                        bd_blocks_for_rd_tail == 0 ? 0 : rows_for_rd_tail,
                        skip_accumulation);
            }
            if (brg.bdb_tail > 0)
                do_ldb_loop(1, true, brg.bdb < 1, true, rows_for_rd_tail,
                        skip_accumulation);
            // for brgemm_strd "no vpadding" case may be implemented, so skip it
            if (brg.type == brgemm_strd) jmp(bdb_loop_end_label);
        }
        if (!vpad_exist || brg.type == brgemm_strd) {
            // for brgemm_strd batch may be null so we need this code path
            L_aligned(no_vpad_label, 64);
            if (brg.bdb > 0) {
                mov(reg_bdb_loop, brg.bdb);
                if (brg.bdb > (rows_for_rd_tail ? 1 : 0)) {
                    Label bdb_loop_label;
                    L_aligned(bdb_loop_label, 64);
                    {
                        bdb_loop_body(1, false, false, false,
                                bd_blocks_for_rd_tail <= 1 ? 0
                                                           : rows_for_rd_tail,
                                skip_accumulation);
                        dec(reg_bdb_loop);
                        cmp(reg_bdb_loop, rows_for_rd_tail ? 1 : 0);
                        jg(bdb_loop_label, T_NEAR);
                    }
                }

                if (rows_for_rd_tail)
                    bdb_loop_body(1, false, false, true,
                            bd_blocks_for_rd_tail == 0 ? 0 : rows_for_rd_tail,
                            skip_accumulation);
            }
            if (brg.bdb_tail > 0)
                do_ldb_loop(1, true, false, false, rows_for_rd_tail,
                        skip_accumulation);
        }
        L_aligned(bdb_loop_end_label, 64);
    };
    auto bdb_loop_amx = [&](bool skip_accumulation) {
        Label bdb_loop_label;
        if (brg.bd_block2 >= 1) {
            mov(reg_bdb_loop, brg.bdb2);
            mov(ptr[rsp + reg_bdb_loop_offs_], reg_bdb_loop);
            L_aligned(bdb_loop_label, 64);
            {
                bdb_loop_body(brg.bd_block2, false, false, false, 0,
                        skip_accumulation);
                mov(reg_bdb_loop, ptr[rsp + reg_bdb_loop_offs_]);
                dec(reg_bdb_loop);
                cmp(reg_bdb_loop, 0);
                mov(ptr[rsp + reg_bdb_loop_offs_], reg_bdb_loop);
            }
            jg(bdb_loop_label, T_NEAR);
        }
        if (brg.bdb2_tail > 0)
            bdb_loop_body(
                    brg.bdb2_tail, false, false, false, 0, skip_accumulation);
        if (brg.bdb_tail > 0)
            do_ldb_loop(1, true, false, false, 0, skip_accumulation);
    };

    auto bdb_loop_general = [&](bool skip_accumulation) {
        if (brg.type == brgemm_addr && brg.brgattr.max_bs == 1 && !vpad_exist
                && !skip_accumulation) {
            mov(reg_aux1_A, ptr[reg_addr_batch + GET_OFF_BATCH_ELEMENT(ptr.A)]);
            mov(reg_aux1_B, ptr[reg_addr_batch + GET_OFF_BATCH_ELEMENT(ptr.B)]);
        }

        xor_(reg_a_offset, reg_a_offset);
        if (brg.is_tmm)
            bdb_loop_amx(skip_accumulation);
        else
            bdb_loop_avx512(skip_accumulation);
    };

    if (brg.brgattr.generate_skip_accumulation) {
        Label bdb_loop_skip_acc_label, bdb_loop_done_label;
        mov(reg_skip_accm, ptr[rsp + reg_skip_accm_offs_]);
        cmp(reg_skip_accm, 0);
        jnz(bdb_loop_skip_acc_label, T_NEAR);

        bdb_loop_general(false);
        jmp(bdb_loop_done_label, T_NEAR);

        L_aligned(bdb_loop_skip_acc_label, 64);
        bdb_loop_general(true);

        L_aligned(bdb_loop_done_label, 64);
    } else
        bdb_loop_general(false);
}

template <typename Wmm>
void jit_brgemm_kernel_t<Wmm>::generate() {
    preamble();

    sub(rsp, stack_space_needed_);

    vpad_exist
            = (brg.brgattr.max_top_vpad > 0 || brg.brgattr.max_bottom_vpad > 0)
            ? true
            : false;
    need_comp_pads = IMPLICATION(brg.zp_type_a == brgemm_broadcast_t::none,
                             brg.req_s8s8_compensation)
            && IMPLICATION(!vpad_exist, brg.req_cal_comp_pads);

    if (is_superset(brg.isa_impl, avx512_core)) {
        const auto full_mask = size_t {0xffffffffffffffff};
        const auto tail_mask = size_t((1 << brg.ldb_tail) - 1);
        reg64_t reg_mask = rax;

        mov(reg_mask, full_mask);
        kmovq(ld_full_mask, reg_mask);
        mov(reg_mask, tail_mask);
        kmovq(ld_tail_mask, reg_mask);
    }

    if (brg.is_int8 && !brg.has_int8_vnni) {
        mov(reg_tmp_gpr.cvt16(), 0x1);
        vpbroadcastw(int8_ones_words(), reg_tmp_gpr.cvt16());
    }

    read_params();

    bdb_loop();

    add(rsp, stack_space_needed_);

    postamble();

    align(32);
    const int simd = vreg_traits<Vmm>::vlen / sizeof(float);
    if (!isa_has_masks(brg.isa_impl) && brg.ldb_tail > 0) {
        L(avx_tail_mask_);
        for (int i = 0; i < brg.ldb_tail; ++i)
            dd(0xffffffff);
        for (int i = brg.ldb_tail; i < simd; ++i)
            dd(0);
    }
    if (!is_superset(brg.isa_impl, avx512_core) && brg.with_sum
            && brg.sum_scale != 1.f) {
        L(sum_zp_scale_data_);
        const int scale_int = float2int(brg.sum_scale);
        for (int i = 0; i < simd; ++i)
            dd(scale_int);
    }

    if (brg.is_fp8_via_convert()) {
        if (f8_e5m2_emulator_) f8_e5m2_emulator_->prepare_table();
        if (f8_e4m3_emulator_) f8_e4m3_emulator_->prepare_table();
    }

    if (brg.with_eltwise)
        postops_injector_->prepare_table(/* generate = */ true);
}

brgemm_attr_t::brgemm_attr_t()
    : max_bs(INT_MAX)
    , max_top_vpad(0)
    , max_bottom_vpad(0)
    , max_top_bpad(0)
    , max_bottom_bpad(0)
    , hint_expected_A_size(platform::get_per_core_cache_size(1))
    , hint_expected_B_size(platform::get_per_core_cache_size(1))
    , hint_expected_C_size(platform::get_per_core_cache_size(1))
    , hint_innermost_loop(brgemm_ld_loop_innermost)
    , hint_loop_order(brgemm_kernel_loop_order_t::brgemm_lo_default)
    , hint_prefetching(brgemm_kernel_prefetching_t::brgemm_prf_default)
    , wary_tail_read(true)
    , generate_skip_accumulation(false)
    , bd_mask_level(0)
    , use_uker(false)
    , use_interleave_stores(false)
    , LDA2(0)
    , LDB2(0)
    , LDC2_M(0)
    , LDC2_N(0)
    , bd_mask(nullptr)
    , static_offsets(nullptr) {}

template <typename Wmm>
brgemm_kernel_common_t<Wmm>::brgemm_kernel_common_t(const brgemm_desc_t &abrd)
    : brgemm_kernel_(new jit_brgemm_kernel_t<Wmm>(abrd)) {}

template <typename Wmm>
status_t brgemm_kernel_common_t<Wmm>::create_kernel() {
    if (brgemm_kernel_) return brgemm_kernel_->create_kernel();
    return status::out_of_memory;
}

template <typename Wmm>
void brgemm_kernel_common_t<Wmm>::operator()(
        brgemm_kernel_params_t *params) const {
    (*brgemm_kernel_)(params);
}

template <typename Wmm>
const jit_generator *brgemm_kernel_common_t<Wmm>::get_jit_generator() const {
    return brgemm_kernel_;
}

template <typename Wmm>
brgemm_kernel_common_t<Wmm>::~brgemm_kernel_common_t() {
    delete brgemm_kernel_;
}

template struct brgemm_kernel_common_t<Xbyak::Tmm>;
template struct brgemm_kernel_common_t<Xbyak::Zmm>;
template struct brgemm_kernel_common_t<Xbyak::Ymm>;
} // namespace x64
} // namespace cpu
} // namespace impl
} // namespace dnnl

// vim: et ts=4 sw=4 cindent cino+=l0,\:4,N-s
