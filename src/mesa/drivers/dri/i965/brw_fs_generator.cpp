/*
 * Copyright © 2010 Intel Corporation
 *
 * Permission is hereby granted, free of charge, to any person obtaining a
 * copy of this software and associated documentation files (the "Software"),
 * to deal in the Software without restriction, including without limitation
 * the rights to use, copy, modify, merge, publish, distribute, sublicense,
 * and/or sell copies of the Software, and to permit persons to whom the
 * Software is furnished to do so, subject to the following conditions:
 *
 * The above copyright notice and this permission notice (including the next
 * paragraph) shall be included in all copies or substantial portions of the
 * Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 * IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT.  IN NO EVENT SHALL
 * THE AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
 * LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING
 * FROM, OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS
 * IN THE SOFTWARE.
 */

/** @file brw_fs_generator.cpp
 *
 * This file supports generating code from the FS LIR to the actual
 * native instructions.
 */

extern "C" {
#include "main/macros.h"
#include "brw_context.h"
#include "brw_eu.h"
} /* extern "C" */

#include "brw_fs.h"
#include "brw_cfg.h"

fs_generator::fs_generator(struct brw_context *brw,
                           void *mem_ctx,
                           const struct brw_wm_prog_key *key,
                           struct brw_wm_prog_data *prog_data,
                           struct gl_shader_program *prog,
                           struct gl_fragment_program *fp,
                           bool dual_source_output,
                           bool runtime_check_aads_emit,
                           bool debug_flag)

   : brw(brw), key(key), prog_data(prog_data), prog(prog), fp(fp),
     dual_source_output(dual_source_output),
     runtime_check_aads_emit(runtime_check_aads_emit), debug_flag(debug_flag),
     mem_ctx(mem_ctx)
{
   ctx = &brw->ctx;

   p = rzalloc(mem_ctx, struct brw_compile);
   brw_init_compile(brw, p, mem_ctx);
}

fs_generator::~fs_generator()
{
}

bool
fs_generator::patch_discard_jumps_to_fb_writes()
{
   if (brw->gen < 6 || this->discard_halt_patches.is_empty())
      return false;

   /* There is a somewhat strange undocumented requirement of using
    * HALT, according to the simulator.  If some channel has HALTed to
    * a particular UIP, then by the end of the program, every channel
    * must have HALTed to that UIP.  Furthermore, the tracking is a
    * stack, so you can't do the final halt of a UIP after starting
    * halting to a new UIP.
    *
    * Symptoms of not emitting this instruction on actual hardware
    * included GPU hangs and sparkly rendering on the piglit discard
    * tests.
    */
   struct brw_instruction *last_halt = gen6_HALT(p);
   last_halt->bits3.break_cont.uip = 2;
   last_halt->bits3.break_cont.jip = 2;

   int ip = p->nr_insn;

   foreach_list(node, &this->discard_halt_patches) {
      ip_record *patch_ip = (ip_record *)node;
      struct brw_instruction *patch = &p->store[patch_ip->ip];

      assert(patch->header.opcode == BRW_OPCODE_HALT);
      /* HALT takes a half-instruction distance from the pre-incremented IP. */
      patch->bits3.break_cont.uip = (ip - patch_ip->ip) * 2;
   }

   this->discard_halt_patches.make_empty();
   return true;
}

void
fs_generator::fire_fb_write(fs_inst *inst,
                            GLuint base_reg,
                            struct brw_reg implied_header,
                            GLuint nr)
{
   uint32_t msg_control;

   if (brw->gen < 6) {
      brw_MOV(p,
              brw_message_reg(base_reg + 1),
              brw_vec8_grf(1, 0));
   }

   if (this->dual_source_output)
      msg_control = BRW_DATAPORT_RENDER_TARGET_WRITE_SIMD8_DUAL_SOURCE_SUBSPAN01;
   else if (dispatch_width == 16)
      msg_control = BRW_DATAPORT_RENDER_TARGET_WRITE_SIMD16_SINGLE_SOURCE;
   else
      msg_control = BRW_DATAPORT_RENDER_TARGET_WRITE_SIMD8_SINGLE_SOURCE_SUBSPAN01;

   uint32_t surf_index =
      prog_data->binding_table.render_target_start + inst->target;

   brw_fb_WRITE(p,
                dispatch_width,
                base_reg,
                implied_header,
                msg_control,
                surf_index,
                nr,
                0,
                inst->eot,
                inst->header_present);

   brw_mark_surface_used(&prog_data->base, surf_index);
}

void
fs_generator::generate_fb_write(fs_inst *inst)
{
   struct brw_reg implied_header;

   /* Header is 2 regs, g0 and g1 are the contents. g0 will be implied
    * move, here's g1.
    */
   brw_push_insn_state(p);
   brw_set_default_mask_control(p, BRW_MASK_DISABLE);
   brw_set_default_predicate_control(p, BRW_PREDICATE_NONE);
   brw_set_default_compression_control(p, BRW_COMPRESSION_NONE);

   if (inst->header_present) {
      /* On HSW, the GPU will use the predicate on SENDC, unless the header is
       * present.
       */
      if ((fp && fp->UsesKill) || key->alpha_test_func) {
         struct brw_reg pixel_mask;

         if (brw->gen >= 6)
            pixel_mask = retype(brw_vec1_grf(1, 7), BRW_REGISTER_TYPE_UW);
         else
            pixel_mask = retype(brw_vec1_grf(0, 0), BRW_REGISTER_TYPE_UW);

         brw_MOV(p, pixel_mask, brw_flag_reg(0, 1));
      }

      if (brw->gen >= 6) {
	 brw_set_default_compression_control(p, BRW_COMPRESSION_COMPRESSED);
	 brw_MOV(p,
		 retype(brw_message_reg(inst->base_mrf), BRW_REGISTER_TYPE_UD),
		 retype(brw_vec8_grf(0, 0), BRW_REGISTER_TYPE_UD));
	 brw_set_default_compression_control(p, BRW_COMPRESSION_NONE);

         if (inst->target > 0 && key->replicate_alpha) {
            /* Set "Source0 Alpha Present to RenderTarget" bit in message
             * header.
             */
            brw_OR(p,
		   vec1(retype(brw_message_reg(inst->base_mrf), BRW_REGISTER_TYPE_UD)),
		   vec1(retype(brw_vec8_grf(0, 0), BRW_REGISTER_TYPE_UD)),
		   brw_imm_ud(0x1 << 11));
         }

	 if (inst->target > 0) {
	    /* Set the render target index for choosing BLEND_STATE. */
	    brw_MOV(p, retype(brw_vec1_reg(BRW_MESSAGE_REGISTER_FILE,
					   inst->base_mrf, 2),
			      BRW_REGISTER_TYPE_UD),
		    brw_imm_ud(inst->target));
	 }

	 implied_header = brw_null_reg();
      } else {
	 implied_header = retype(brw_vec8_grf(0, 0), BRW_REGISTER_TYPE_UW);
      }
   } else {
      implied_header = brw_null_reg();
   }

   if (!runtime_check_aads_emit) {
      fire_fb_write(inst, inst->base_mrf, implied_header, inst->mlen);
   } else {
      /* This can only happen in gen < 6 */
      assert(brw->gen < 6);

      struct brw_reg v1_null_ud = vec1(retype(brw_null_reg(), BRW_REGISTER_TYPE_UD));

      /* Check runtime bit to detect if we have to send AA data or not */
      brw_set_default_compression_control(p, BRW_COMPRESSION_NONE);
      brw_AND(p,
              v1_null_ud,
              retype(brw_vec1_grf(1, 6), BRW_REGISTER_TYPE_UD),
              brw_imm_ud(1<<26));
      brw_last_inst->header.destreg__conditionalmod = BRW_CONDITIONAL_NZ;

      int jmp = brw_JMPI(p, brw_imm_ud(0), BRW_PREDICATE_NORMAL) - p->store;
      brw_last_inst->header.execution_size = BRW_EXECUTE_1;
      {
         /* Don't send AA data */
         fire_fb_write(inst, inst->base_mrf+1, implied_header, inst->mlen-1);
      }
      brw_land_fwd_jump(p, jmp);
      fire_fb_write(inst, inst->base_mrf, implied_header, inst->mlen);
   }

   brw_pop_insn_state(p);
}

void
fs_generator::generate_blorp_fb_write(fs_inst *inst)
{
   brw_fb_WRITE(p,
                16 /* dispatch_width */,
                inst->base_mrf,
                brw_reg_from_fs_reg(&inst->src[0]),
                BRW_DATAPORT_RENDER_TARGET_WRITE_SIMD16_SINGLE_SOURCE,
                inst->target,
                inst->mlen,
                0,
                true,
                inst->header_present);
}

/* Computes the integer pixel x,y values from the origin.
 *
 * This is the basis of gl_FragCoord computation, but is also used
 * pre-gen6 for computing the deltas from v0 for computing
 * interpolation.
 */
void
fs_generator::generate_pixel_xy(struct brw_reg dst, bool is_x)
{
   struct brw_reg g1_uw = retype(brw_vec1_grf(1, 0), BRW_REGISTER_TYPE_UW);
   struct brw_reg src;
   struct brw_reg deltas;

   if (is_x) {
      src = stride(suboffset(g1_uw, 4), 2, 4, 0);
      deltas = brw_imm_v(0x10101010);
   } else {
      src = stride(suboffset(g1_uw, 5), 2, 4, 0);
      deltas = brw_imm_v(0x11001100);
   }

   if (dispatch_width == 16) {
      dst = vec16(dst);
   }

   /* We do this SIMD8 or SIMD16, but since the destination is UW we
    * don't do compression in the SIMD16 case.
    */
   brw_push_insn_state(p);
   brw_set_default_compression_control(p, BRW_COMPRESSION_NONE);
   brw_ADD(p, dst, src, deltas);
   brw_pop_insn_state(p);
}

void
fs_generator::generate_linterp(fs_inst *inst,
			     struct brw_reg dst, struct brw_reg *src)
{
   struct brw_reg delta_x = src[0];
   struct brw_reg delta_y = src[1];
   struct brw_reg interp = src[2];

   if (brw->has_pln &&
       delta_y.nr == delta_x.nr + 1 &&
       (brw->gen >= 6 || (delta_x.nr & 1) == 0)) {
      brw_PLN(p, dst, interp, delta_x);
   } else {
      brw_LINE(p, brw_null_reg(), interp, delta_x);
      brw_MAC(p, dst, suboffset(interp, 1), delta_y);
   }
}

void
fs_generator::generate_math1_gen7(fs_inst *inst,
			        struct brw_reg dst,
			        struct brw_reg src0)
{
   assert(inst->mlen == 0);
   brw_math(p, dst,
	    brw_math_function(inst->opcode),
	    0, src0,
	    BRW_MATH_DATA_VECTOR,
	    BRW_MATH_PRECISION_FULL);
}

void
fs_generator::generate_math2_gen7(fs_inst *inst,
			        struct brw_reg dst,
			        struct brw_reg src0,
			        struct brw_reg src1)
{
   assert(inst->mlen == 0);
   brw_math2(p, dst, brw_math_function(inst->opcode), src0, src1);
}

void
fs_generator::generate_math1_gen6(fs_inst *inst,
			        struct brw_reg dst,
			        struct brw_reg src0)
{
   int op = brw_math_function(inst->opcode);

   assert(inst->mlen == 0);

   brw_set_default_compression_control(p, BRW_COMPRESSION_NONE);
   brw_math(p, dst,
	    op,
	    0, src0,
	    BRW_MATH_DATA_VECTOR,
	    BRW_MATH_PRECISION_FULL);

   if (dispatch_width == 16) {
      brw_set_default_compression_control(p, BRW_COMPRESSION_2NDHALF);
      brw_math(p, sechalf(dst),
	       op,
	       0, sechalf(src0),
	       BRW_MATH_DATA_VECTOR,
	       BRW_MATH_PRECISION_FULL);
      brw_set_default_compression_control(p, BRW_COMPRESSION_COMPRESSED);
   }
}

void
fs_generator::generate_math2_gen6(fs_inst *inst,
			        struct brw_reg dst,
			        struct brw_reg src0,
			        struct brw_reg src1)
{
   int op = brw_math_function(inst->opcode);

   assert(inst->mlen == 0);

   brw_set_default_compression_control(p, BRW_COMPRESSION_NONE);
   brw_math2(p, dst, op, src0, src1);

   if (dispatch_width == 16) {
      brw_set_default_compression_control(p, BRW_COMPRESSION_2NDHALF);
      brw_math2(p, sechalf(dst), op, sechalf(src0), sechalf(src1));
      brw_set_default_compression_control(p, BRW_COMPRESSION_COMPRESSED);
   }
}

void
fs_generator::generate_math_gen4(fs_inst *inst,
			       struct brw_reg dst,
			       struct brw_reg src)
{
   int op = brw_math_function(inst->opcode);

   assert(inst->mlen >= 1);

   brw_set_default_compression_control(p, BRW_COMPRESSION_NONE);
   brw_math(p, dst,
	    op,
	    inst->base_mrf, src,
	    BRW_MATH_DATA_VECTOR,
	    BRW_MATH_PRECISION_FULL);

   if (dispatch_width == 16) {
      brw_set_default_compression_control(p, BRW_COMPRESSION_2NDHALF);
      brw_math(p, sechalf(dst),
	       op,
	       inst->base_mrf + 1, sechalf(src),
	       BRW_MATH_DATA_VECTOR,
	       BRW_MATH_PRECISION_FULL);

      brw_set_default_compression_control(p, BRW_COMPRESSION_COMPRESSED);
   }
}

void
fs_generator::generate_math_g45(fs_inst *inst,
                                struct brw_reg dst,
                                struct brw_reg src)
{
   if (inst->opcode == SHADER_OPCODE_POW ||
       inst->opcode == SHADER_OPCODE_INT_QUOTIENT ||
       inst->opcode == SHADER_OPCODE_INT_REMAINDER) {
      generate_math_gen4(inst, dst, src);
      return;
   }

   int op = brw_math_function(inst->opcode);

   assert(inst->mlen >= 1);

   brw_math(p, dst,
            op,
            inst->base_mrf, src,
            BRW_MATH_DATA_VECTOR,
            BRW_MATH_PRECISION_FULL);
}

void
fs_generator::generate_tex(fs_inst *inst, struct brw_reg dst, struct brw_reg src)
{
   int msg_type = -1;
   int rlen = 4;
   uint32_t simd_mode = BRW_SAMPLER_SIMD_MODE_SIMD8;
   uint32_t return_format;

   switch (dst.type) {
   case BRW_REGISTER_TYPE_D:
      return_format = BRW_SAMPLER_RETURN_FORMAT_SINT32;
      break;
   case BRW_REGISTER_TYPE_UD:
      return_format = BRW_SAMPLER_RETURN_FORMAT_UINT32;
      break;
   default:
      return_format = BRW_SAMPLER_RETURN_FORMAT_FLOAT32;
      break;
   }

   if (dispatch_width == 16 &&
      !inst->force_uncompressed && !inst->force_sechalf)
      simd_mode = BRW_SAMPLER_SIMD_MODE_SIMD16;

   if (brw->gen >= 5) {
      switch (inst->opcode) {
      case SHADER_OPCODE_TEX:
	 if (inst->shadow_compare) {
	    msg_type = GEN5_SAMPLER_MESSAGE_SAMPLE_COMPARE;
	 } else {
	    msg_type = GEN5_SAMPLER_MESSAGE_SAMPLE;
	 }
	 break;
      case FS_OPCODE_TXB:
	 if (inst->shadow_compare) {
	    msg_type = GEN5_SAMPLER_MESSAGE_SAMPLE_BIAS_COMPARE;
	 } else {
	    msg_type = GEN5_SAMPLER_MESSAGE_SAMPLE_BIAS;
	 }
	 break;
      case SHADER_OPCODE_TXL:
	 if (inst->shadow_compare) {
	    msg_type = GEN5_SAMPLER_MESSAGE_SAMPLE_LOD_COMPARE;
	 } else {
	    msg_type = GEN5_SAMPLER_MESSAGE_SAMPLE_LOD;
	 }
	 break;
      case SHADER_OPCODE_TXS:
	 msg_type = GEN5_SAMPLER_MESSAGE_SAMPLE_RESINFO;
	 break;
      case SHADER_OPCODE_TXD:
         if (inst->shadow_compare) {
            /* Gen7.5+.  Otherwise, lowered by brw_lower_texture_gradients(). */
            assert(brw->is_haswell);
            msg_type = HSW_SAMPLER_MESSAGE_SAMPLE_DERIV_COMPARE;
         } else {
            msg_type = GEN5_SAMPLER_MESSAGE_SAMPLE_DERIVS;
         }
	 break;
      case SHADER_OPCODE_TXF:
	 msg_type = GEN5_SAMPLER_MESSAGE_SAMPLE_LD;
	 break;
      case SHADER_OPCODE_TXF_CMS:
         if (brw->gen >= 7)
            msg_type = GEN7_SAMPLER_MESSAGE_SAMPLE_LD2DMS;
         else
            msg_type = GEN5_SAMPLER_MESSAGE_SAMPLE_LD;
         break;
      case SHADER_OPCODE_TXF_UMS:
         assert(brw->gen >= 7);
         msg_type = GEN7_SAMPLER_MESSAGE_SAMPLE_LD2DSS;
         break;
      case SHADER_OPCODE_TXF_MCS:
         assert(brw->gen >= 7);
         msg_type = GEN7_SAMPLER_MESSAGE_SAMPLE_LD_MCS;
         break;
      case SHADER_OPCODE_LOD:
         msg_type = GEN5_SAMPLER_MESSAGE_LOD;
         break;
      case SHADER_OPCODE_TG4:
         if (inst->shadow_compare) {
            assert(brw->gen >= 7);
            msg_type = GEN7_SAMPLER_MESSAGE_SAMPLE_GATHER4_C;
         } else {
            assert(brw->gen >= 6);
            msg_type = GEN7_SAMPLER_MESSAGE_SAMPLE_GATHER4;
         }
         break;
      case SHADER_OPCODE_TG4_OFFSET:
         assert(brw->gen >= 7);
         if (inst->shadow_compare) {
            msg_type = GEN7_SAMPLER_MESSAGE_SAMPLE_GATHER4_PO_C;
         } else {
            msg_type = GEN7_SAMPLER_MESSAGE_SAMPLE_GATHER4_PO;
         }
         break;
      default:
	 assert(!"not reached");
	 break;
      }
   } else {
      switch (inst->opcode) {
      case SHADER_OPCODE_TEX:
	 /* Note that G45 and older determines shadow compare and dispatch width
	  * from message length for most messages.
	  */
	 assert(dispatch_width == 8);
	 msg_type = BRW_SAMPLER_MESSAGE_SIMD8_SAMPLE;
	 if (inst->shadow_compare) {
	    assert(inst->mlen == 6);
	 } else {
	    assert(inst->mlen <= 4);
	 }
	 break;
      case FS_OPCODE_TXB:
	 if (inst->shadow_compare) {
	    assert(inst->mlen == 6);
	    msg_type = BRW_SAMPLER_MESSAGE_SIMD8_SAMPLE_BIAS_COMPARE;
	 } else {
	    assert(inst->mlen == 9);
	    msg_type = BRW_SAMPLER_MESSAGE_SIMD16_SAMPLE_BIAS;
	    simd_mode = BRW_SAMPLER_SIMD_MODE_SIMD16;
	 }
	 break;
      case SHADER_OPCODE_TXL:
	 if (inst->shadow_compare) {
	    assert(inst->mlen == 6);
	    msg_type = BRW_SAMPLER_MESSAGE_SIMD8_SAMPLE_LOD_COMPARE;
	 } else {
	    assert(inst->mlen == 9);
	    msg_type = BRW_SAMPLER_MESSAGE_SIMD16_SAMPLE_LOD;
	    simd_mode = BRW_SAMPLER_SIMD_MODE_SIMD16;
	 }
	 break;
      case SHADER_OPCODE_TXD:
	 /* There is no sample_d_c message; comparisons are done manually */
	 assert(inst->mlen == 7 || inst->mlen == 10);
	 msg_type = BRW_SAMPLER_MESSAGE_SIMD8_SAMPLE_GRADIENTS;
	 break;
      case SHADER_OPCODE_TXF:
	 assert(inst->mlen == 9);
	 msg_type = BRW_SAMPLER_MESSAGE_SIMD16_LD;
	 simd_mode = BRW_SAMPLER_SIMD_MODE_SIMD16;
	 break;
      case SHADER_OPCODE_TXS:
	 assert(inst->mlen == 3);
	 msg_type = BRW_SAMPLER_MESSAGE_SIMD16_RESINFO;
	 simd_mode = BRW_SAMPLER_SIMD_MODE_SIMD16;
	 break;
      default:
	 assert(!"not reached");
	 break;
      }
   }
   assert(msg_type != -1);

   if (simd_mode == BRW_SAMPLER_SIMD_MODE_SIMD16) {
      rlen = 8;
      dst = vec16(dst);
   }

   if (brw->gen >= 7 && inst->header_present && dispatch_width == 16) {
      /* The send-from-GRF for SIMD16 texturing with a header has an extra
       * hardware register allocated to it, which we need to skip over (since
       * our coordinates in the payload are in the even-numbered registers,
       * and the header comes right before the first one).
       */
      assert(src.file == BRW_GENERAL_REGISTER_FILE);
      src.nr++;
   }

   /* Load the message header if present.  If there's a texture offset,
    * we need to set it up explicitly and load the offset bitfield.
    * Otherwise, we can use an implied move from g0 to the first message reg.
    */
   if (inst->header_present) {
      if (brw->gen < 6 && !inst->texture_offset) {
         /* Set up an implied move from g0 to the MRF. */
         src = retype(brw_vec8_grf(0, 0), BRW_REGISTER_TYPE_UW);
      } else {
         struct brw_reg header_reg;

         if (brw->gen >= 7) {
            header_reg = src;
         } else {
            assert(inst->base_mrf != -1);
            header_reg = brw_message_reg(inst->base_mrf);
         }

         brw_push_insn_state(p);
         brw_set_default_mask_control(p, BRW_MASK_DISABLE);
         brw_set_default_compression_control(p, BRW_COMPRESSION_NONE);
         /* Explicitly set up the message header by copying g0 to the MRF. */
         brw_MOV(p, header_reg, brw_vec8_grf(0, 0));

         if (inst->texture_offset) {
            /* Set the offset bits in DWord 2. */
            brw_MOV(p, get_element_ud(header_reg, 2),
                       brw_imm_ud(inst->texture_offset));
         }

         if (inst->sampler >= 16) {
            /* The "Sampler Index" field can only store values between 0 and 15.
             * However, we can add an offset to the "Sampler State Pointer"
             * field, effectively selecting a different set of 16 samplers.
             *
             * The "Sampler State Pointer" needs to be aligned to a 32-byte
             * offset, and each sampler state is only 16-bytes, so we can't
             * exclusively use the offset - we have to use both.
             */
            assert(brw->is_haswell); /* field only exists on Haswell */
            brw_ADD(p,
                    get_element_ud(header_reg, 3),
                    get_element_ud(brw_vec8_grf(0, 0), 3),
                    brw_imm_ud(16 * (inst->sampler / 16) *
                               sizeof(gen7_sampler_state)));
         }
         brw_pop_insn_state(p);
      }
   }

   uint32_t surface_index = ((inst->opcode == SHADER_OPCODE_TG4 ||
      inst->opcode == SHADER_OPCODE_TG4_OFFSET)
      ? prog_data->base.binding_table.gather_texture_start
      : prog_data->base.binding_table.texture_start) + inst->sampler;

   brw_SAMPLE(p,
	      retype(dst, BRW_REGISTER_TYPE_UW),
	      inst->base_mrf,
	      src,
              surface_index,
	      inst->sampler % 16,
	      msg_type,
	      rlen,
	      inst->mlen,
	      inst->header_present,
	      simd_mode,
	      return_format);

   brw_mark_surface_used(&prog_data->base, surface_index);
}


/* For OPCODE_DDX and OPCODE_DDY, per channel of output we've got input
 * looking like:
 *
 * arg0: ss0.tl ss0.tr ss0.bl ss0.br ss1.tl ss1.tr ss1.bl ss1.br
 *
 * Ideally, we want to produce:
 *
 *           DDX                     DDY
 * dst: (ss0.tr - ss0.tl)     (ss0.tl - ss0.bl)
 *      (ss0.tr - ss0.tl)     (ss0.tr - ss0.br)
 *      (ss0.br - ss0.bl)     (ss0.tl - ss0.bl)
 *      (ss0.br - ss0.bl)     (ss0.tr - ss0.br)
 *      (ss1.tr - ss1.tl)     (ss1.tl - ss1.bl)
 *      (ss1.tr - ss1.tl)     (ss1.tr - ss1.br)
 *      (ss1.br - ss1.bl)     (ss1.tl - ss1.bl)
 *      (ss1.br - ss1.bl)     (ss1.tr - ss1.br)
 *
 * and add another set of two more subspans if in 16-pixel dispatch mode.
 *
 * For DDX, it ends up being easy: width = 2, horiz=0 gets us the same result
 * for each pair, and vertstride = 2 jumps us 2 elements after processing a
 * pair.  But the ideal approximation may impose a huge performance cost on
 * sample_d.  On at least Haswell, sample_d instruction does some
 * optimizations if the same LOD is used for all pixels in the subspan.
 *
 * For DDY, we need to use ALIGN16 mode since it's capable of doing the
 * appropriate swizzling.
 */
void
fs_generator::generate_ddx(fs_inst *inst, struct brw_reg dst, struct brw_reg src)
{
   unsigned vstride, width;

   if (key->high_quality_derivatives) {
      /* produce accurate derivatives */
      vstride = BRW_VERTICAL_STRIDE_2;
      width = BRW_WIDTH_2;
   }
   else {
      /* replicate the derivative at the top-left pixel to other pixels */
      vstride = BRW_VERTICAL_STRIDE_4;
      width = BRW_WIDTH_4;
   }

   struct brw_reg src0 = brw_reg(src.file, src.nr, 1,
				 BRW_REGISTER_TYPE_F,
				 vstride,
				 width,
				 BRW_HORIZONTAL_STRIDE_0,
				 BRW_SWIZZLE_XYZW, WRITEMASK_XYZW);
   struct brw_reg src1 = brw_reg(src.file, src.nr, 0,
				 BRW_REGISTER_TYPE_F,
				 vstride,
				 width,
				 BRW_HORIZONTAL_STRIDE_0,
				 BRW_SWIZZLE_XYZW, WRITEMASK_XYZW);
   brw_ADD(p, dst, src0, negate(src1));
}

/* The negate_value boolean is used to negate the derivative computation for
 * FBOs, since they place the origin at the upper left instead of the lower
 * left.
 */
void
fs_generator::generate_ddy(fs_inst *inst, struct brw_reg dst, struct brw_reg src,
                         bool negate_value)
{
   if (key->high_quality_derivatives) {
      /* From the Ivy Bridge PRM, volume 4 part 3, section 3.3.9 (Register
       * Region Restrictions):
       *
       *     In Align16 access mode, SIMD16 is not allowed for DW operations
       *     and SIMD8 is not allowed for DF operations.
       *
       * In this context, "DW operations" means "operations acting on 32-bit
       * values", so it includes operations on floats.
       *
       * Gen4 has a similar restriction.  From the i965 PRM, section 11.5.3
       * (Instruction Compression -> Rules and Restrictions):
       *
       *     A compressed instruction must be in Align1 access mode. Align16
       *     mode instructions cannot be compressed.
       *
       * Similar text exists in the g45 PRM.
       *
       * On these platforms, if we're building a SIMD16 shader, we need to
       * manually unroll to a pair of SIMD8 instructions.
       */
      bool unroll_to_simd8 =
         (dispatch_width == 16 &&
          (brw->gen == 4 || (brw->gen == 7 && !brw->is_haswell)));

      /* produce accurate derivatives */
      struct brw_reg src0 = brw_reg(src.file, src.nr, 0,
                                    BRW_REGISTER_TYPE_F,
                                    BRW_VERTICAL_STRIDE_4,
                                    BRW_WIDTH_4,
                                    BRW_HORIZONTAL_STRIDE_1,
                                    BRW_SWIZZLE_XYXY, WRITEMASK_XYZW);
      struct brw_reg src1 = brw_reg(src.file, src.nr, 0,
                                    BRW_REGISTER_TYPE_F,
                                    BRW_VERTICAL_STRIDE_4,
                                    BRW_WIDTH_4,
                                    BRW_HORIZONTAL_STRIDE_1,
                                    BRW_SWIZZLE_ZWZW, WRITEMASK_XYZW);
      brw_push_insn_state(p);
      brw_set_default_access_mode(p, BRW_ALIGN_16);
      if (unroll_to_simd8)
         brw_set_default_compression_control(p, BRW_COMPRESSION_NONE);
      if (negate_value)
         brw_ADD(p, dst, src1, negate(src0));
      else
         brw_ADD(p, dst, src0, negate(src1));
      if (unroll_to_simd8) {
         brw_set_default_compression_control(p, BRW_COMPRESSION_2NDHALF);
         src0 = sechalf(src0);
         src1 = sechalf(src1);
         dst = sechalf(dst);
         if (negate_value)
            brw_ADD(p, dst, src1, negate(src0));
         else
            brw_ADD(p, dst, src0, negate(src1));
      }
      brw_pop_insn_state(p);
   } else {
      /* replicate the derivative at the top-left pixel to other pixels */
      struct brw_reg src0 = brw_reg(src.file, src.nr, 0,
                                    BRW_REGISTER_TYPE_F,
                                    BRW_VERTICAL_STRIDE_4,
                                    BRW_WIDTH_4,
                                    BRW_HORIZONTAL_STRIDE_0,
                                    BRW_SWIZZLE_XYZW, WRITEMASK_XYZW);
      struct brw_reg src1 = brw_reg(src.file, src.nr, 2,
                                    BRW_REGISTER_TYPE_F,
                                    BRW_VERTICAL_STRIDE_4,
                                    BRW_WIDTH_4,
                                    BRW_HORIZONTAL_STRIDE_0,
                                    BRW_SWIZZLE_XYZW, WRITEMASK_XYZW);
      if (negate_value)
         brw_ADD(p, dst, src1, negate(src0));
      else
         brw_ADD(p, dst, src0, negate(src1));
   }
}

void
fs_generator::generate_discard_jump(fs_inst *inst)
{
   assert(brw->gen >= 6);

   /* This HALT will be patched up at FB write time to point UIP at the end of
    * the program, and at brw_uip_jip() JIP will be set to the end of the
    * current block (or the program).
    */
   this->discard_halt_patches.push_tail(new(mem_ctx) ip_record(p->nr_insn));

   brw_push_insn_state(p);
   brw_set_default_mask_control(p, BRW_MASK_DISABLE);
   gen6_HALT(p);
   brw_pop_insn_state(p);
}

void
fs_generator::generate_scratch_write(fs_inst *inst, struct brw_reg src)
{
   assert(inst->mlen != 0);

   brw_MOV(p,
	   retype(brw_message_reg(inst->base_mrf + 1), BRW_REGISTER_TYPE_UD),
	   retype(src, BRW_REGISTER_TYPE_UD));
   brw_oword_block_write_scratch(p, brw_message_reg(inst->base_mrf),
                                 dispatch_width / 8, inst->offset);
}

void
fs_generator::generate_scratch_read(fs_inst *inst, struct brw_reg dst)
{
   assert(inst->mlen != 0);

   brw_oword_block_read_scratch(p, dst, brw_message_reg(inst->base_mrf),
                                dispatch_width / 8, inst->offset);
}

void
fs_generator::generate_scratch_read_gen7(fs_inst *inst, struct brw_reg dst)
{
   gen7_block_read_scratch(p, dst, dispatch_width / 8, inst->offset);
}

void
fs_generator::generate_uniform_pull_constant_load(fs_inst *inst,
                                                  struct brw_reg dst,
                                                  struct brw_reg index,
                                                  struct brw_reg offset)
{
   assert(inst->mlen != 0);

   assert(index.file == BRW_IMMEDIATE_VALUE &&
	  index.type == BRW_REGISTER_TYPE_UD);
   uint32_t surf_index = index.dw1.ud;

   assert(offset.file == BRW_IMMEDIATE_VALUE &&
	  offset.type == BRW_REGISTER_TYPE_UD);
   uint32_t read_offset = offset.dw1.ud;

   brw_oword_block_read(p, dst, brw_message_reg(inst->base_mrf),
			read_offset, surf_index);

   brw_mark_surface_used(&prog_data->base, surf_index);
}

void
fs_generator::generate_uniform_pull_constant_load_gen7(fs_inst *inst,
                                                       struct brw_reg dst,
                                                       struct brw_reg index,
                                                       struct brw_reg offset)
{
   assert(inst->mlen == 0);

   assert(index.file == BRW_IMMEDIATE_VALUE &&
	  index.type == BRW_REGISTER_TYPE_UD);
   uint32_t surf_index = index.dw1.ud;

   assert(offset.file == BRW_GENERAL_REGISTER_FILE);
   /* Reference just the dword we need, to avoid angering validate_reg(). */
   offset = brw_vec1_grf(offset.nr, 0);

   brw_push_insn_state(p);
   brw_set_default_compression_control(p, BRW_COMPRESSION_NONE);
   brw_set_default_mask_control(p, BRW_MASK_DISABLE);
   struct brw_instruction *send = brw_next_insn(p, BRW_OPCODE_SEND);
   brw_pop_insn_state(p);

   /* We use the SIMD4x2 mode because we want to end up with 4 components in
    * the destination loaded consecutively from the same offset (which appears
    * in the first component, and the rest are ignored).
    */
   dst.width = BRW_WIDTH_4;
   brw_set_dest(p, send, dst);
   brw_set_src0(p, send, offset);
   brw_set_sampler_message(p, send,
                           surf_index,
                           0, /* LD message ignores sampler unit */
                           GEN5_SAMPLER_MESSAGE_SAMPLE_LD,
                           1, /* rlen */
                           1, /* mlen */
                           false, /* no header */
                           BRW_SAMPLER_SIMD_MODE_SIMD4X2,
                           0);

   brw_mark_surface_used(&prog_data->base, surf_index);
}

void
fs_generator::generate_varying_pull_constant_load(fs_inst *inst,
                                                  struct brw_reg dst,
                                                  struct brw_reg index,
                                                  struct brw_reg offset)
{
   assert(brw->gen < 7); /* Should use the gen7 variant. */
   assert(inst->header_present);
   assert(inst->mlen);

   assert(index.file == BRW_IMMEDIATE_VALUE &&
	  index.type == BRW_REGISTER_TYPE_UD);
   uint32_t surf_index = index.dw1.ud;

   uint32_t simd_mode, rlen, msg_type;
   if (dispatch_width == 16) {
      simd_mode = BRW_SAMPLER_SIMD_MODE_SIMD16;
      rlen = 8;
   } else {
      simd_mode = BRW_SAMPLER_SIMD_MODE_SIMD8;
      rlen = 4;
   }

   if (brw->gen >= 5)
      msg_type = GEN5_SAMPLER_MESSAGE_SAMPLE_LD;
   else {
      /* We always use the SIMD16 message so that we only have to load U, and
       * not V or R.
       */
      msg_type = BRW_SAMPLER_MESSAGE_SIMD16_LD;
      assert(inst->mlen == 3);
      assert(inst->regs_written == 8);
      rlen = 8;
      simd_mode = BRW_SAMPLER_SIMD_MODE_SIMD16;
   }

   struct brw_reg offset_mrf = retype(brw_message_reg(inst->base_mrf + 1),
                                      BRW_REGISTER_TYPE_D);
   brw_MOV(p, offset_mrf, offset);

   struct brw_reg header = brw_vec8_grf(0, 0);
   gen6_resolve_implied_move(p, &header, inst->base_mrf);

   struct brw_instruction *send = brw_next_insn(p, BRW_OPCODE_SEND);
   send->header.compression_control = BRW_COMPRESSION_NONE;
   brw_set_dest(p, send, retype(dst, BRW_REGISTER_TYPE_UW));
   brw_set_src0(p, send, header);
   if (brw->gen < 6)
      send->header.destreg__conditionalmod = inst->base_mrf;

   /* Our surface is set up as floats, regardless of what actual data is
    * stored in it.
    */
   uint32_t return_format = BRW_SAMPLER_RETURN_FORMAT_FLOAT32;
   brw_set_sampler_message(p, send,
                           surf_index,
                           0, /* sampler (unused) */
                           msg_type,
                           rlen,
                           inst->mlen,
                           inst->header_present,
                           simd_mode,
                           return_format);

   brw_mark_surface_used(&prog_data->base, surf_index);
}

void
fs_generator::generate_varying_pull_constant_load_gen7(fs_inst *inst,
                                                       struct brw_reg dst,
                                                       struct brw_reg index,
                                                       struct brw_reg offset)
{
   assert(brw->gen >= 7);
   /* Varying-offset pull constant loads are treated as a normal expression on
    * gen7, so the fact that it's a send message is hidden at the IR level.
    */
   assert(!inst->header_present);
   assert(!inst->mlen);

   assert(index.file == BRW_IMMEDIATE_VALUE &&
	  index.type == BRW_REGISTER_TYPE_UD);
   uint32_t surf_index = index.dw1.ud;

   uint32_t simd_mode, rlen, mlen;
   if (dispatch_width == 16) {
      mlen = 2;
      rlen = 8;
      simd_mode = BRW_SAMPLER_SIMD_MODE_SIMD16;
   } else {
      mlen = 1;
      rlen = 4;
      simd_mode = BRW_SAMPLER_SIMD_MODE_SIMD8;
   }

   struct brw_instruction *send = brw_next_insn(p, BRW_OPCODE_SEND);
   brw_set_dest(p, send, dst);
   brw_set_src0(p, send, offset);
   brw_set_sampler_message(p, send,
                           surf_index,
                           0, /* LD message ignores sampler unit */
                           GEN5_SAMPLER_MESSAGE_SAMPLE_LD,
                           rlen,
                           mlen,
                           false, /* no header */
                           simd_mode,
                           0);

   brw_mark_surface_used(&prog_data->base, surf_index);
}

/**
 * Cause the current pixel/sample mask (from R1.7 bits 15:0) to be transferred
 * into the flags register (f0.0).
 *
 * Used only on Gen6 and above.
 */
void
fs_generator::generate_mov_dispatch_to_flags(fs_inst *inst)
{
   struct brw_reg flags = brw_flag_reg(0, inst->flag_subreg);
   struct brw_reg dispatch_mask;

   if (brw->gen >= 6)
      dispatch_mask = retype(brw_vec1_grf(1, 7), BRW_REGISTER_TYPE_UW);
   else
      dispatch_mask = retype(brw_vec1_grf(0, 0), BRW_REGISTER_TYPE_UW);

   brw_push_insn_state(p);
   brw_set_default_mask_control(p, BRW_MASK_DISABLE);
   brw_MOV(p, flags, dispatch_mask);
   brw_pop_insn_state(p);
}


static uint32_t brw_file_from_reg(fs_reg *reg)
{
   switch (reg->file) {
   case GRF:
      return BRW_GENERAL_REGISTER_FILE;
   case MRF:
      return BRW_MESSAGE_REGISTER_FILE;
   case IMM:
      return BRW_IMMEDIATE_VALUE;
   default:
      assert(!"not reached");
      return BRW_GENERAL_REGISTER_FILE;
   }
}

struct brw_reg
brw_reg_from_fs_reg(fs_reg *reg)
{
   struct brw_reg brw_reg;

   switch (reg->file) {
   case GRF:
   case MRF:
      if (reg->stride == 0) {
         brw_reg = brw_vec1_reg(brw_file_from_reg(reg), reg->reg, 0);
      } else {
         brw_reg = brw_vec8_reg(brw_file_from_reg(reg), reg->reg, 0);
         brw_reg = stride(brw_reg, 8 * reg->stride, 8, reg->stride);
      }

      brw_reg = retype(brw_reg, reg->type);
      brw_reg = byte_offset(brw_reg, reg->subreg_offset);
      break;
   case IMM:
      switch (reg->type) {
      case BRW_REGISTER_TYPE_F:
	 brw_reg = brw_imm_f(reg->imm.f);
	 break;
      case BRW_REGISTER_TYPE_D:
	 brw_reg = brw_imm_d(reg->imm.i);
	 break;
      case BRW_REGISTER_TYPE_UD:
	 brw_reg = brw_imm_ud(reg->imm.u);
	 break;
      default:
	 assert(!"not reached");
	 brw_reg = brw_null_reg();
	 break;
      }
      break;
   case HW_REG:
      assert(reg->type == reg->fixed_hw_reg.type);
      brw_reg = reg->fixed_hw_reg;
      break;
   case BAD_FILE:
      /* Probably unused. */
      brw_reg = brw_null_reg();
      break;
   case UNIFORM:
      assert(!"not reached");
      brw_reg = brw_null_reg();
      break;
   default:
      assert(!"not reached");
      brw_reg = brw_null_reg();
      break;
   }
   if (reg->abs)
      brw_reg = brw_abs(brw_reg);
   if (reg->negate)
      brw_reg = negate(brw_reg);

   return brw_reg;
}

/**
 * Sets the first word of a vgrf for gen7+ simd4x2 uniform pull constant
 * sampler LD messages.
 *
 * We don't want to bake it into the send message's code generation because
 * that means we don't get a chance to schedule the instructions.
 */
void
fs_generator::generate_set_simd4x2_offset(fs_inst *inst,
                                          struct brw_reg dst,
                                          struct brw_reg value)
{
   assert(value.file == BRW_IMMEDIATE_VALUE);

   brw_push_insn_state(p);
   brw_set_default_compression_control(p, BRW_COMPRESSION_NONE);
   brw_set_default_mask_control(p, BRW_MASK_DISABLE);
   brw_MOV(p, retype(brw_vec1_reg(dst.file, dst.nr, 0), value.type), value);
   brw_pop_insn_state(p);
}

/* Sets vstride=16, width=8, hstride=2 or vstride=0, width=1, hstride=0
 * (when mask is passed as a uniform) of register mask before moving it
 * to register dst.
 */
void
fs_generator::generate_set_omask(fs_inst *inst,
                                 struct brw_reg dst,
                                 struct brw_reg mask)
{
   bool stride_8_8_1 =
    (mask.vstride == BRW_VERTICAL_STRIDE_8 &&
     mask.width == BRW_WIDTH_8 &&
     mask.hstride == BRW_HORIZONTAL_STRIDE_1);

   bool stride_0_1_0 =
    (mask.vstride == BRW_VERTICAL_STRIDE_0 &&
     mask.width == BRW_WIDTH_1 &&
     mask.hstride == BRW_HORIZONTAL_STRIDE_0);

   assert(stride_8_8_1 || stride_0_1_0);
   assert(dst.type == BRW_REGISTER_TYPE_UW);

   if (dispatch_width == 16)
      dst = vec16(dst);
   brw_push_insn_state(p);
   brw_set_default_compression_control(p, BRW_COMPRESSION_NONE);
   brw_set_default_mask_control(p, BRW_MASK_DISABLE);

   if (stride_8_8_1) {
      brw_MOV(p, dst, retype(stride(mask, 16, 8, 2), dst.type));
   } else if (stride_0_1_0) {
      brw_MOV(p, dst, retype(mask, dst.type));
   }
   brw_pop_insn_state(p);
}

/* Sets vstride=1, width=4, hstride=0 of register src1 during
 * the ADD instruction.
 */
void
fs_generator::generate_set_sample_id(fs_inst *inst,
                                     struct brw_reg dst,
                                     struct brw_reg src0,
                                     struct brw_reg src1)
{
   assert(dst.type == BRW_REGISTER_TYPE_D ||
          dst.type == BRW_REGISTER_TYPE_UD);
   assert(src0.type == BRW_REGISTER_TYPE_D ||
          src0.type == BRW_REGISTER_TYPE_UD);

   brw_push_insn_state(p);
   brw_set_default_compression_control(p, BRW_COMPRESSION_NONE);
   brw_set_default_mask_control(p, BRW_MASK_DISABLE);
   struct brw_reg reg = retype(stride(src1, 1, 4, 0), BRW_REGISTER_TYPE_UW);
   brw_ADD(p, dst, src0, reg);
   if (dispatch_width == 16)
      brw_ADD(p, offset(dst, 1), offset(src0, 1), suboffset(reg, 2));
   brw_pop_insn_state(p);
}

/**
 * Change the register's data type from UD to W, doubling the strides in order
 * to compensate for halving the data type width.
 */
static struct brw_reg
ud_reg_to_w(struct brw_reg r)
{
   assert(r.type == BRW_REGISTER_TYPE_UD);
   r.type = BRW_REGISTER_TYPE_W;

   /* The BRW_*_STRIDE enums are defined so that incrementing the field
    * doubles the real stride.
    */
   if (r.hstride != 0)
      ++r.hstride;
   if (r.vstride != 0)
      ++r.vstride;

   return r;
}

void
fs_generator::generate_pack_half_2x16_split(fs_inst *inst,
                                            struct brw_reg dst,
                                            struct brw_reg x,
                                            struct brw_reg y)
{
   assert(brw->gen >= 7);
   assert(dst.type == BRW_REGISTER_TYPE_UD);
   assert(x.type == BRW_REGISTER_TYPE_F);
   assert(y.type == BRW_REGISTER_TYPE_F);

   /* From the Ivybridge PRM, Vol4, Part3, Section 6.27 f32to16:
    *
    *   Because this instruction does not have a 16-bit floating-point type,
    *   the destination data type must be Word (W).
    *
    *   The destination must be DWord-aligned and specify a horizontal stride
    *   (HorzStride) of 2. The 16-bit result is stored in the lower word of
    *   each destination channel and the upper word is not modified.
    */
   struct brw_reg dst_w = ud_reg_to_w(dst);

   /* Give each 32-bit channel of dst the form below , where "." means
    * unchanged.
    *   0x....hhhh
    */
   brw_F32TO16(p, dst_w, y);

   /* Now the form:
    *   0xhhhh0000
    */
   brw_SHL(p, dst, dst, brw_imm_ud(16u));

   /* And, finally the form of packHalf2x16's output:
    *   0xhhhhllll
    */
   brw_F32TO16(p, dst_w, x);
}

void
fs_generator::generate_unpack_half_2x16_split(fs_inst *inst,
                                              struct brw_reg dst,
                                              struct brw_reg src)
{
   assert(brw->gen >= 7);
   assert(dst.type == BRW_REGISTER_TYPE_F);
   assert(src.type == BRW_REGISTER_TYPE_UD);

   /* From the Ivybridge PRM, Vol4, Part3, Section 6.26 f16to32:
    *
    *   Because this instruction does not have a 16-bit floating-point type,
    *   the source data type must be Word (W). The destination type must be
    *   F (Float).
    */
   struct brw_reg src_w = ud_reg_to_w(src);

   /* Each channel of src has the form of unpackHalf2x16's input: 0xhhhhllll.
    * For the Y case, we wish to access only the upper word; therefore
    * a 16-bit subregister offset is needed.
    */
   assert(inst->opcode == FS_OPCODE_UNPACK_HALF_2x16_SPLIT_X ||
          inst->opcode == FS_OPCODE_UNPACK_HALF_2x16_SPLIT_Y);
   if (inst->opcode == FS_OPCODE_UNPACK_HALF_2x16_SPLIT_Y)
      src_w.subnr += 2;

   brw_F16TO32(p, dst, src_w);
}

void
fs_generator::generate_shader_time_add(fs_inst *inst,
                                       struct brw_reg payload,
                                       struct brw_reg offset,
                                       struct brw_reg value)
{
   assert(brw->gen >= 7);
   brw_push_insn_state(p);
   brw_set_default_mask_control(p, true);

   assert(payload.file == BRW_GENERAL_REGISTER_FILE);
   struct brw_reg payload_offset = retype(brw_vec1_grf(payload.nr, 0),
                                          offset.type);
   struct brw_reg payload_value = retype(brw_vec1_grf(payload.nr + 1, 0),
                                         value.type);

   assert(offset.file == BRW_IMMEDIATE_VALUE);
   if (value.file == BRW_GENERAL_REGISTER_FILE) {
      value.width = BRW_WIDTH_1;
      value.hstride = BRW_HORIZONTAL_STRIDE_0;
      value.vstride = BRW_VERTICAL_STRIDE_0;
   } else {
      assert(value.file == BRW_IMMEDIATE_VALUE);
   }

   /* Trying to deal with setup of the params from the IR is crazy in the FS8
    * case, and we don't really care about squeezing every bit of performance
    * out of this path, so we just emit the MOVs from here.
    */
   brw_MOV(p, payload_offset, offset);
   brw_MOV(p, payload_value, value);
   brw_shader_time_add(p, payload,
                       prog_data->base.binding_table.shader_time_start);
   brw_pop_insn_state(p);

   brw_mark_surface_used(&prog_data->base,
                         prog_data->base.binding_table.shader_time_start);
}

void
fs_generator::generate_untyped_atomic(fs_inst *inst, struct brw_reg dst,
                                      struct brw_reg atomic_op,
                                      struct brw_reg surf_index)
{
   assert(atomic_op.file == BRW_IMMEDIATE_VALUE &&
          atomic_op.type == BRW_REGISTER_TYPE_UD &&
          surf_index.file == BRW_IMMEDIATE_VALUE &&
	  surf_index.type == BRW_REGISTER_TYPE_UD);

   brw_untyped_atomic(p, dst, brw_message_reg(inst->base_mrf),
                      atomic_op.dw1.ud, surf_index.dw1.ud,
                      inst->mlen, dispatch_width / 8);

   brw_mark_surface_used(&prog_data->base, surf_index.dw1.ud);
}

void
fs_generator::generate_untyped_surface_read(fs_inst *inst, struct brw_reg dst,
                                            struct brw_reg surf_index)
{
   assert(surf_index.file == BRW_IMMEDIATE_VALUE &&
	  surf_index.type == BRW_REGISTER_TYPE_UD);

   brw_untyped_surface_read(p, dst, brw_message_reg(inst->base_mrf),
                            surf_index.dw1.ud,
                            inst->mlen, dispatch_width / 8);

   brw_mark_surface_used(&prog_data->base, surf_index.dw1.ud);
}

void
fs_generator::generate_code(exec_list *instructions)
{
   int start_offset = p->next_insn_offset;

   struct annotation_info annotation;
   memset(&annotation, 0, sizeof(annotation));

   cfg_t *cfg = NULL;
   if (unlikely(debug_flag))
      cfg = new(mem_ctx) cfg_t(instructions);

   foreach_list(node, instructions) {
      fs_inst *inst = (fs_inst *)node;
      struct brw_reg src[3], dst;
      unsigned int last_insn_offset = p->next_insn_offset;

      if (unlikely(debug_flag))
         annotate(brw, &annotation, cfg, inst, p->next_insn_offset);

      for (unsigned int i = 0; i < inst->sources; i++) {
	 src[i] = brw_reg_from_fs_reg(&inst->src[i]);

	 /* The accumulator result appears to get used for the
	  * conditional modifier generation.  When negating a UD
	  * value, there is a 33rd bit generated for the sign in the
	  * accumulator value, so now you can't check, for example,
	  * equality with a 32-bit value.  See piglit fs-op-neg-uvec4.
	  */
	 assert(!inst->conditional_mod ||
		inst->src[i].type != BRW_REGISTER_TYPE_UD ||
		!inst->src[i].negate);
      }
      dst = brw_reg_from_fs_reg(&inst->dst);

      brw_set_default_predicate_control(p, inst->predicate);
      brw_set_default_predicate_inverse(p, inst->predicate_inverse);
      brw_set_default_flag_reg(p, 0, inst->flag_subreg);
      brw_set_default_saturate(p, inst->saturate);
      brw_set_default_mask_control(p, inst->force_writemask_all);
      brw_set_default_acc_write_control(p, inst->writes_accumulator);

      if (inst->force_uncompressed || dispatch_width == 8) {
	 brw_set_default_compression_control(p, BRW_COMPRESSION_NONE);
      } else if (inst->force_sechalf) {
	 brw_set_default_compression_control(p, BRW_COMPRESSION_2NDHALF);
      } else {
	 brw_set_default_compression_control(p, BRW_COMPRESSION_COMPRESSED);
      }

      switch (inst->opcode) {
      case BRW_OPCODE_MOV:
	 brw_MOV(p, dst, src[0]);
	 break;
      case BRW_OPCODE_ADD:
	 brw_ADD(p, dst, src[0], src[1]);
	 break;
      case BRW_OPCODE_MUL:
	 brw_MUL(p, dst, src[0], src[1]);
	 break;
      case BRW_OPCODE_AVG:
	 brw_AVG(p, dst, src[0], src[1]);
	 break;
      case BRW_OPCODE_MACH:
	 brw_MACH(p, dst, src[0], src[1]);
	 break;

      case BRW_OPCODE_MAD:
         assert(brw->gen >= 6);
	 brw_set_default_access_mode(p, BRW_ALIGN_16);
         if (dispatch_width == 16 && !brw->is_haswell) {
	    brw_set_default_compression_control(p, BRW_COMPRESSION_NONE);
	    brw_MAD(p, dst, src[0], src[1], src[2]);
	    brw_set_default_compression_control(p, BRW_COMPRESSION_2NDHALF);
	    brw_MAD(p, sechalf(dst), sechalf(src[0]), sechalf(src[1]), sechalf(src[2]));
	    brw_set_default_compression_control(p, BRW_COMPRESSION_COMPRESSED);
	 } else {
	    brw_MAD(p, dst, src[0], src[1], src[2]);
	 }
	 brw_set_default_access_mode(p, BRW_ALIGN_1);
	 break;

      case BRW_OPCODE_LRP:
         assert(brw->gen >= 6);
	 brw_set_default_access_mode(p, BRW_ALIGN_16);
         if (dispatch_width == 16 && !brw->is_haswell) {
	    brw_set_default_compression_control(p, BRW_COMPRESSION_NONE);
	    brw_LRP(p, dst, src[0], src[1], src[2]);
	    brw_set_default_compression_control(p, BRW_COMPRESSION_2NDHALF);
	    brw_LRP(p, sechalf(dst), sechalf(src[0]), sechalf(src[1]), sechalf(src[2]));
	    brw_set_default_compression_control(p, BRW_COMPRESSION_COMPRESSED);
	 } else {
	    brw_LRP(p, dst, src[0], src[1], src[2]);
	 }
	 brw_set_default_access_mode(p, BRW_ALIGN_1);
	 break;

      case BRW_OPCODE_FRC:
	 brw_FRC(p, dst, src[0]);
	 break;
      case BRW_OPCODE_RNDD:
	 brw_RNDD(p, dst, src[0]);
	 break;
      case BRW_OPCODE_RNDE:
	 brw_RNDE(p, dst, src[0]);
	 break;
      case BRW_OPCODE_RNDZ:
	 brw_RNDZ(p, dst, src[0]);
	 break;

      case BRW_OPCODE_AND:
	 brw_AND(p, dst, src[0], src[1]);
	 break;
      case BRW_OPCODE_OR:
	 brw_OR(p, dst, src[0], src[1]);
	 break;
      case BRW_OPCODE_XOR:
	 brw_XOR(p, dst, src[0], src[1]);
	 break;
      case BRW_OPCODE_NOT:
	 brw_NOT(p, dst, src[0]);
	 break;
      case BRW_OPCODE_ASR:
	 brw_ASR(p, dst, src[0], src[1]);
	 break;
      case BRW_OPCODE_SHR:
	 brw_SHR(p, dst, src[0], src[1]);
	 break;
      case BRW_OPCODE_SHL:
	 brw_SHL(p, dst, src[0], src[1]);
	 break;
      case BRW_OPCODE_F32TO16:
         assert(brw->gen >= 7);
         brw_F32TO16(p, dst, src[0]);
         break;
      case BRW_OPCODE_F16TO32:
         assert(brw->gen >= 7);
         brw_F16TO32(p, dst, src[0]);
         break;
      case BRW_OPCODE_CMP:
	 brw_CMP(p, dst, inst->conditional_mod, src[0], src[1]);
	 break;
      case BRW_OPCODE_SEL:
	 brw_SEL(p, dst, src[0], src[1]);
	 break;
      case BRW_OPCODE_BFREV:
         assert(brw->gen >= 7);
         /* BFREV only supports UD type for src and dst. */
         brw_BFREV(p, retype(dst, BRW_REGISTER_TYPE_UD),
                      retype(src[0], BRW_REGISTER_TYPE_UD));
         break;
      case BRW_OPCODE_FBH:
         assert(brw->gen >= 7);
         /* FBH only supports UD type for dst. */
         brw_FBH(p, retype(dst, BRW_REGISTER_TYPE_UD), src[0]);
         break;
      case BRW_OPCODE_FBL:
         assert(brw->gen >= 7);
         /* FBL only supports UD type for dst. */
         brw_FBL(p, retype(dst, BRW_REGISTER_TYPE_UD), src[0]);
         break;
      case BRW_OPCODE_CBIT:
         assert(brw->gen >= 7);
         /* CBIT only supports UD type for dst. */
         brw_CBIT(p, retype(dst, BRW_REGISTER_TYPE_UD), src[0]);
         break;
      case BRW_OPCODE_ADDC:
         assert(brw->gen >= 7);
         brw_ADDC(p, dst, src[0], src[1]);
         break;
      case BRW_OPCODE_SUBB:
         assert(brw->gen >= 7);
         brw_SUBB(p, dst, src[0], src[1]);
         break;
      case BRW_OPCODE_MAC:
         brw_MAC(p, dst, src[0], src[1]);
         break;

      case BRW_OPCODE_BFE:
         assert(brw->gen >= 7);
         brw_set_default_access_mode(p, BRW_ALIGN_16);
         if (dispatch_width == 16 && !brw->is_haswell) {
            brw_set_default_compression_control(p, BRW_COMPRESSION_NONE);
            brw_BFE(p, dst, src[0], src[1], src[2]);
            brw_set_default_compression_control(p, BRW_COMPRESSION_2NDHALF);
            brw_BFE(p, sechalf(dst), sechalf(src[0]), sechalf(src[1]), sechalf(src[2]));
            brw_set_default_compression_control(p, BRW_COMPRESSION_COMPRESSED);
         } else {
            brw_BFE(p, dst, src[0], src[1], src[2]);
         }
         brw_set_default_access_mode(p, BRW_ALIGN_1);
         break;

      case BRW_OPCODE_BFI1:
         assert(brw->gen >= 7);
         /* The Haswell WaForceSIMD8ForBFIInstruction workaround says that we
          * should
          *
          *    "Force BFI instructions to be executed always in SIMD8."
          */
         if (dispatch_width == 16 && brw->is_haswell) {
            brw_set_default_compression_control(p, BRW_COMPRESSION_NONE);
            brw_BFI1(p, dst, src[0], src[1]);
            brw_set_default_compression_control(p, BRW_COMPRESSION_2NDHALF);
            brw_BFI1(p, sechalf(dst), sechalf(src[0]), sechalf(src[1]));
            brw_set_default_compression_control(p, BRW_COMPRESSION_COMPRESSED);
         } else {
            brw_BFI1(p, dst, src[0], src[1]);
         }
         break;
      case BRW_OPCODE_BFI2:
         assert(brw->gen >= 7);
         brw_set_default_access_mode(p, BRW_ALIGN_16);
         /* The Haswell WaForceSIMD8ForBFIInstruction workaround says that we
          * should
          *
          *    "Force BFI instructions to be executed always in SIMD8."
          *
          * Otherwise we would be able to emit compressed instructions like we
          * do for the other three-source instructions.
          */
         if (dispatch_width == 16) {
            brw_set_default_compression_control(p, BRW_COMPRESSION_NONE);
            brw_BFI2(p, dst, src[0], src[1], src[2]);
            brw_set_default_compression_control(p, BRW_COMPRESSION_2NDHALF);
            brw_BFI2(p, sechalf(dst), sechalf(src[0]), sechalf(src[1]), sechalf(src[2]));
            brw_set_default_compression_control(p, BRW_COMPRESSION_COMPRESSED);
         } else {
            brw_BFI2(p, dst, src[0], src[1], src[2]);
         }
         brw_set_default_access_mode(p, BRW_ALIGN_1);
         break;

      case BRW_OPCODE_IF:
	 if (inst->src[0].file != BAD_FILE) {
	    /* The instruction has an embedded compare (only allowed on gen6) */
	    assert(brw->gen == 6);
	    gen6_IF(p, inst->conditional_mod, src[0], src[1]);
	 } else {
	    brw_IF(p, dispatch_width == 16 ? BRW_EXECUTE_16 : BRW_EXECUTE_8);
	 }
	 break;

      case BRW_OPCODE_ELSE:
	 brw_ELSE(p);
	 break;
      case BRW_OPCODE_ENDIF:
	 brw_ENDIF(p);
	 break;

      case BRW_OPCODE_DO:
	 brw_DO(p, BRW_EXECUTE_8);
	 break;

      case BRW_OPCODE_BREAK:
	 brw_BREAK(p);
	 brw_set_default_predicate_control(p, BRW_PREDICATE_NONE);
	 break;
      case BRW_OPCODE_CONTINUE:
	 /* FINISHME: We need to write the loop instruction support still. */
	 if (brw->gen >= 6)
	    gen6_CONT(p);
	 else
	    brw_CONT(p);
	 brw_set_default_predicate_control(p, BRW_PREDICATE_NONE);
	 break;

      case BRW_OPCODE_WHILE:
	 brw_WHILE(p);
	 break;

      case SHADER_OPCODE_RCP:
      case SHADER_OPCODE_RSQ:
      case SHADER_OPCODE_SQRT:
      case SHADER_OPCODE_EXP2:
      case SHADER_OPCODE_LOG2:
      case SHADER_OPCODE_SIN:
      case SHADER_OPCODE_COS:
	 if (brw->gen >= 7) {
	    generate_math1_gen7(inst, dst, src[0]);
	 } else if (brw->gen == 6) {
	    generate_math1_gen6(inst, dst, src[0]);
	 } else if (brw->gen == 5 || brw->is_g4x) {
	    generate_math_g45(inst, dst, src[0]);
	 } else {
	    generate_math_gen4(inst, dst, src[0]);
	 }
	 break;
      case SHADER_OPCODE_INT_QUOTIENT:
      case SHADER_OPCODE_INT_REMAINDER:
      case SHADER_OPCODE_POW:
	 if (brw->gen >= 7) {
	    generate_math2_gen7(inst, dst, src[0], src[1]);
	 } else if (brw->gen == 6) {
	    generate_math2_gen6(inst, dst, src[0], src[1]);
	 } else {
	    generate_math_gen4(inst, dst, src[0]);
	 }
	 break;
      case FS_OPCODE_PIXEL_X:
	 generate_pixel_xy(dst, true);
	 break;
      case FS_OPCODE_PIXEL_Y:
	 generate_pixel_xy(dst, false);
	 break;
      case FS_OPCODE_CINTERP:
	 brw_MOV(p, dst, src[0]);
	 break;
      case FS_OPCODE_LINTERP:
	 generate_linterp(inst, dst, src);
	 break;
      case SHADER_OPCODE_TEX:
      case FS_OPCODE_TXB:
      case SHADER_OPCODE_TXD:
      case SHADER_OPCODE_TXF:
      case SHADER_OPCODE_TXF_CMS:
      case SHADER_OPCODE_TXF_UMS:
      case SHADER_OPCODE_TXF_MCS:
      case SHADER_OPCODE_TXL:
      case SHADER_OPCODE_TXS:
      case SHADER_OPCODE_LOD:
      case SHADER_OPCODE_TG4:
      case SHADER_OPCODE_TG4_OFFSET:
	 generate_tex(inst, dst, src[0]);
	 break;
      case FS_OPCODE_DDX:
	 generate_ddx(inst, dst, src[0]);
	 break;
      case FS_OPCODE_DDY:
         /* Make sure fp->UsesDFdy flag got set (otherwise there's no
          * guarantee that key->render_to_fbo is set).
          */
         assert(fp->UsesDFdy);
	 generate_ddy(inst, dst, src[0], key->render_to_fbo);
	 break;

      case SHADER_OPCODE_GEN4_SCRATCH_WRITE:
	 generate_scratch_write(inst, src[0]);
	 break;

      case SHADER_OPCODE_GEN4_SCRATCH_READ:
	 generate_scratch_read(inst, dst);
	 break;

      case SHADER_OPCODE_GEN7_SCRATCH_READ:
	 generate_scratch_read_gen7(inst, dst);
	 break;

      case FS_OPCODE_UNIFORM_PULL_CONSTANT_LOAD:
	 generate_uniform_pull_constant_load(inst, dst, src[0], src[1]);
	 break;

      case FS_OPCODE_UNIFORM_PULL_CONSTANT_LOAD_GEN7:
	 generate_uniform_pull_constant_load_gen7(inst, dst, src[0], src[1]);
	 break;

      case FS_OPCODE_VARYING_PULL_CONSTANT_LOAD:
	 generate_varying_pull_constant_load(inst, dst, src[0], src[1]);
	 break;

      case FS_OPCODE_VARYING_PULL_CONSTANT_LOAD_GEN7:
	 generate_varying_pull_constant_load_gen7(inst, dst, src[0], src[1]);
	 break;

      case FS_OPCODE_FB_WRITE:
	 generate_fb_write(inst);
	 break;

      case FS_OPCODE_BLORP_FB_WRITE:
	 generate_blorp_fb_write(inst);
	 break;

      case FS_OPCODE_MOV_DISPATCH_TO_FLAGS:
         generate_mov_dispatch_to_flags(inst);
         break;

      case FS_OPCODE_DISCARD_JUMP:
         generate_discard_jump(inst);
         break;

      case SHADER_OPCODE_SHADER_TIME_ADD:
         generate_shader_time_add(inst, src[0], src[1], src[2]);
         break;

      case SHADER_OPCODE_UNTYPED_ATOMIC:
         generate_untyped_atomic(inst, dst, src[0], src[1]);
         break;

      case SHADER_OPCODE_UNTYPED_SURFACE_READ:
         generate_untyped_surface_read(inst, dst, src[0]);
         break;

      case FS_OPCODE_SET_SIMD4X2_OFFSET:
         generate_set_simd4x2_offset(inst, dst, src[0]);
         break;

      case FS_OPCODE_SET_OMASK:
         generate_set_omask(inst, dst, src[0]);
         break;

      case FS_OPCODE_SET_SAMPLE_ID:
         generate_set_sample_id(inst, dst, src[0], src[1]);
         break;

      case FS_OPCODE_PACK_HALF_2x16_SPLIT:
          generate_pack_half_2x16_split(inst, dst, src[0], src[1]);
          break;

      case FS_OPCODE_UNPACK_HALF_2x16_SPLIT_X:
      case FS_OPCODE_UNPACK_HALF_2x16_SPLIT_Y:
         generate_unpack_half_2x16_split(inst, dst, src[0]);
         break;

      case FS_OPCODE_PLACEHOLDER_HALT:
         /* This is the place where the final HALT needs to be inserted if
          * we've emitted any discards.  If not, this will emit no code.
          */
         if (!patch_discard_jumps_to_fb_writes()) {
            if (unlikely(debug_flag)) {
               annotation.ann_count--;
            }
         }
         break;

      default:
	 if (inst->opcode < (int) ARRAY_SIZE(opcode_descs)) {
	    _mesa_problem(ctx, "Unsupported opcode `%s' in FS",
			  opcode_descs[inst->opcode].name);
	 } else {
	    _mesa_problem(ctx, "Unsupported opcode %d in FS", inst->opcode);
	 }
	 abort();
      }

      if (inst->conditional_mod) {
         /* Set the conditional modifier on the last instruction we generated.
          * Also, make sure we only emitted one instruction - anything else
          * doesn't make sense.
          */
         assert(p->next_insn_offset == last_insn_offset + 16);
         struct brw_instruction *last = &p->store[last_insn_offset / 16];
         last->header.destreg__conditionalmod = inst->conditional_mod;
      }
   }

   brw_set_uip_jip(p);
   annotation_finalize(&annotation, p->next_insn_offset);

   int before_size = p->next_insn_offset - start_offset;
   brw_compact_instructions(p, start_offset, annotation.ann_count,
                            annotation.ann);
   int after_size = p->next_insn_offset - start_offset;

   if (unlikely(debug_flag)) {
      if (prog) {
         fprintf(stderr,
                 "Native code for %s fragment shader %d (SIMD%d dispatch):\n",
                 prog->Label ? prog->Label : "unnamed",
                 prog->Name, dispatch_width);
      } else if (fp) {
         fprintf(stderr,
                 "Native code for fragment program %d (SIMD%d dispatch):\n",
                 fp->Base.Id, dispatch_width);
      } else {
         fprintf(stderr, "Native code for blorp program (SIMD%d dispatch):\n",
                 dispatch_width);
      }
      fprintf(stderr, "SIMD%d shader: %d instructions. Compacted %d to %d"
                      " bytes (%.0f%%)\n",
              dispatch_width, before_size / 16, before_size, after_size,
              100.0f * (before_size - after_size) / before_size);

      const struct gl_program *prog = fp ? &fp->Base : NULL;

      dump_assembly(p->store, annotation.ann_count, annotation.ann,
                    brw, prog, brw_disassemble);
      ralloc_free(annotation.ann);
   }
}

const unsigned *
fs_generator::generate_assembly(exec_list *simd8_instructions,
                                exec_list *simd16_instructions,
                                unsigned *assembly_size)
{
   assert(simd8_instructions || simd16_instructions);

   if (simd8_instructions) {
      dispatch_width = 8;
      generate_code(simd8_instructions);
   }

   if (simd16_instructions) {
      /* align to 64 byte boundary. */
      while (p->next_insn_offset % 64) {
         brw_NOP(p);
      }

      /* Save off the start of this SIMD16 program */
      prog_data->prog_offset_16 = p->next_insn_offset;

      brw_set_default_compression_control(p, BRW_COMPRESSION_COMPRESSED);

      dispatch_width = 16;
      generate_code(simd16_instructions);
   }

   return brw_get_program(p, assembly_size);
}
