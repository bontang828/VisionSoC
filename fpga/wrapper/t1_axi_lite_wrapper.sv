//This is the the T1 AXI Lite Wrapper, it bridges ARM PS (AXI4-Lite master) to T1 vector processor issue/retire interface.
//This module is independent to the internal of T1's high level parameters, so shuold work as long as the interface dont change

//Works as following:
// PS fills the instruction bits/meta into the corresponding address of the interface register
// Then PS write into the CTRL to trigger an Issue for T1 to read


//RO: read only, RW: read write, W1S: write bit 1 to set, W1C: Write bit 1 ro clear
//Register Map (32-bit registers, byte-addressed):
//  0x00   CTRL      [0] W1S: issue_start (auto clears on handshake)
//                   [1] RO:  issue_ready (T1 can accept)
//                   [2] RO:  issue_busy  (issue in progress, not yet accepted)
//  0x04   INSTRUCTION   RW:  32-bit RISC-V V instruction encoding
//  0x08   RS1_DATA      RW:  scalar RS1 operand
//  0x0C   RS2_DATA      RW:  scalar RS2 operand
//  0x10   VTYPE         RW:  vtype CSR (standard [7:0] + custom upper bits)
//  0x14   VL            RW:  vector length
//  0x18   VSTART        RW:  vector start
//  0x1C   VCSR          RW:  vector CSR (vxrm, vxsat + custom upper bits)
//  0x20   RD_FIFO_STS   RO:  [3:0] rd FIFO count, [4] rd FIFO non-empty
//  0x24   RD_POP_DATA   RO:  read pops rd FIFO - returns rdData[31:0]
//  0x28   RD_POP_META   RO:  [4:0] rdAddress, [5] isFp, Fp is not support at the moment
//  0x2C   CSR_FIFO_STS  RO:  [3:0] csr FIFO count, [4] csr FIFO non-empty
//  0x30   CSR_POP       RO:  read pops csr FIFO - returns vxsat[31:0]
//  0x34   CSR_FFLAG     RO:  fflag from last csr FIFO pop
//  0x38   MEM_COUNT     RO:  [7:0] saturating count of mem retire events, W1C to decrement
//  0x3C   IRQ_EN        RW:  [0] enable IRQ on rd FIFO non-empty
//                            [1] enable IRQ on csr FIFO non-empty
//                            [2] enable IRQ on mem_count > 0
//  0x40   IRQ_STATUS    RO:  [0] rd pending, [1] csr pending, [2] mem pending




module t1_axi_lite_wrapper #(
    parameter int ADDR_WIDTH  = 7,//128 bytes register space (17 registers)
    parameter int DATA_WIDTH  = 32,
    parameter int RD_FIFO_DEPTH  = 4, //match chainingSize, current design have chaining no more than 4 instructions
    parameter int CSR_FIFO_DEPTH = 4
) (
    input  logic                    aclk,
    input  logic                    aresetn,

    //AXI4-Lite Slave Interface
    input  logic [ADDR_WIDTH-1:0]   s_axi_awaddr,
    input  logic [2:0]              s_axi_awprot,
    input  logic                    s_axi_awvalid,
    output logic                    s_axi_awready,

    input  logic [DATA_WIDTH-1:0]   s_axi_wdata,
    input  logic [DATA_WIDTH/8-1:0] s_axi_wstrb,
    input  logic                    s_axi_wvalid,
    output logic                    s_axi_wready,

    output logic [1:0]              s_axi_bresp,
    output logic                    s_axi_bvalid,
    input  logic                    s_axi_bready,

    input  logic [ADDR_WIDTH-1:0]   s_axi_araddr,
    input  logic [2:0]              s_axi_arprot,
    input  logic                    s_axi_arvalid,
    output logic                    s_axi_arready,

    output logic [DATA_WIDTH-1:0]   s_axi_rdata,
    output logic [1:0]              s_axi_rresp,
    output logic                    s_axi_rvalid,
    input  logic                    s_axi_rready,

    //T1 Issue Interface (directly to T1 ports)
    output logic                    issue_valid,
    input  logic                    issue_ready,
    output logic [31:0]             issue_bits_instruction,
    output logic [31:0]             issue_bits_rs1Data,
    output logic [31:0]             issue_bits_rs2Data,
    output logic [31:0]             issue_bits_vtype,
    output logic [31:0]             issue_bits_vl,
    output logic [31:0]             issue_bits_vstart,
    output logic [31:0]             issue_bits_vcsr,

    //T1 Retire Interface (directly from T1 ports)
    input  logic                    retire_rd_valid,
    input  logic [4:0]              retire_rd_bits_rdAddress,
    input  logic [31:0]             retire_rd_bits_rdData,
    input  logic                    retire_rd_bits_isFp,
    input  logic                    retire_csr_valid,
    input  logic [31:0]             retire_csr_bits_vxsat,
    input  logic [31:0]             retire_csr_bits_fflag,
    input  logic                    retire_mem_valid,

    //Interrupt to PS
    output logic                    irq
);

    //Issue Registers
    logic [31:0] reg_instruction;
    logic [31:0] reg_rs1_data;
    logic [31:0] reg_rs2_data;
    logic [31:0] reg_vtype;
    logic [31:0] reg_vl;
    logic [31:0] reg_vstart;
    logic [31:0] reg_vcsr;
    logic        issue_pending;  //issue_valid held high until ready

    //Drive T1 issue port
    assign issue_valid           = issue_pending;
    assign issue_bits_instruction = reg_instruction;
    assign issue_bits_rs1Data    = reg_rs1_data;
    assign issue_bits_rs2Data    = reg_rs2_data;
    assign issue_bits_vtype      = reg_vtype;
    assign issue_bits_vl         = reg_vl;
    assign issue_bits_vstart     = reg_vstart;
    assign issue_bits_vcsr       = reg_vcsr;
    //issue_pending is driven by the CTRL register write block below







    //Retire RD FIFO
    localparam int RD_FIFO_W = 38;  //32 data + 5 addr + 1 isFp
    logic [RD_FIFO_W-1:0] rd_fifo_mem [RD_FIFO_DEPTH];
    logic [$clog2(RD_FIFO_DEPTH):0] rd_fifo_wptr, rd_fifo_rptr, rd_fifo_count;
    logic rd_fifo_push, rd_fifo_pop;
    logic rd_fifo_empty, rd_fifo_full;
    logic [RD_FIFO_W-1:0] rd_fifo_dout;

    assign rd_fifo_count = rd_fifo_wptr - rd_fifo_rptr;
    assign rd_fifo_empty = (rd_fifo_count == 0);
    assign rd_fifo_full  = (rd_fifo_count == RD_FIFO_DEPTH[$clog2(RD_FIFO_DEPTH):0]);

    assign rd_fifo_push = retire_rd_valid && !rd_fifo_full;
    assign rd_fifo_dout = rd_fifo_mem[rd_fifo_rptr[$clog2(RD_FIFO_DEPTH)-1:0]];

    always_ff @(posedge aclk or negedge aresetn) begin
        if (!aresetn) begin
            rd_fifo_wptr <= '0;
        end else if (rd_fifo_push) begin
            rd_fifo_mem[rd_fifo_wptr[$clog2(RD_FIFO_DEPTH)-1:0]] <=
                {retire_rd_bits_isFp, retire_rd_bits_rdAddress, retire_rd_bits_rdData};
            rd_fifo_wptr <= rd_fifo_wptr + 1;
        end
    end

    always_ff @(posedge aclk or negedge aresetn) begin
        if (!aresetn)
            rd_fifo_rptr <= '0;
        else if (rd_fifo_pop && !rd_fifo_empty)
            rd_fifo_rptr <= rd_fifo_rptr + 1;
    end







    //Retire CSR FIFO
    localparam int CSR_FIFO_W = 64;  //32 vxsat + 32 fflag <- just for compatibility with T1 current retire interface, realistically only [0] bit of vxsat is needed as we dont support float
    logic [CSR_FIFO_W-1:0] csr_fifo_mem [CSR_FIFO_DEPTH];
    logic [$clog2(CSR_FIFO_DEPTH):0] csr_fifo_wptr, csr_fifo_rptr, csr_fifo_count;
    logic csr_fifo_push, csr_fifo_pop;
    logic csr_fifo_empty, csr_fifo_full;
    logic [CSR_FIFO_W-1:0] csr_fifo_dout;

    assign csr_fifo_count = csr_fifo_wptr - csr_fifo_rptr;
    assign csr_fifo_empty = (csr_fifo_count == 0);
    assign csr_fifo_full  = (csr_fifo_count == CSR_FIFO_DEPTH[$clog2(CSR_FIFO_DEPTH):0]);

    assign csr_fifo_push = retire_csr_valid && !csr_fifo_full;
    assign csr_fifo_dout = csr_fifo_mem[csr_fifo_rptr[$clog2(CSR_FIFO_DEPTH)-1:0]];

    always_ff @(posedge aclk or negedge aresetn) begin
        if (!aresetn) begin
            csr_fifo_wptr <= '0;
        end else if (csr_fifo_push) begin
            csr_fifo_mem[csr_fifo_wptr[$clog2(CSR_FIFO_DEPTH)-1:0]] <=
                {retire_csr_bits_fflag, retire_csr_bits_vxsat};
            csr_fifo_wptr <= csr_fifo_wptr + 1;
        end
    end

    always_ff @(posedge aclk or negedge aresetn) begin
        if (!aresetn)
            csr_fifo_rptr <= '0;
        else if (csr_fifo_pop && !csr_fifo_empty)
            csr_fifo_rptr <= csr_fifo_rptr + 1;
    end

    //Latch rd metad What ita alongside rd FIFO pop
    logic [5:0] rd_meta_latched;  //{isFp, rdAddress[4:0]}
    always_ff @(posedge aclk or negedge aresetn) begin
        if (!aresetn)
            rd_meta_latched <= '0;
        else if (rd_fifo_pop && !rd_fifo_empty)
            rd_meta_latched <= rd_fifo_dout[37:32];
    end

    //Latch fflag alongside vxsat pop
    logic [31:0] csr_fflag_latched;
    always_ff @(posedge aclk or negedge aresetn) begin
        if (!aresetn)
            csr_fflag_latched <= '0;
        else if (csr_fifo_pop && !csr_fifo_empty)
            csr_fflag_latched <= csr_fifo_dout[63:32];
    end






    //Retire MEM Counter (saturating)
    logic [7:0] mem_count;
    logic       mem_count_dec;  //W1C from register write

    always_ff @(posedge aclk or negedge aresetn) begin
        if (!aresetn)
            mem_count <= '0;
        else begin
            case ({retire_mem_valid, mem_count_dec})
                2'b10:   if (mem_count != 8'hFF) mem_count <= mem_count + 1;
                2'b01:   if (mem_count != 8'h00) mem_count <= mem_count - 1;
                2'b11:   mem_count <= mem_count; // inc + dec cancel
                default: mem_count <= mem_count;
            endcase
        end
    end






    // IRQ Logic
    logic [2:0] reg_irq_en;
    logic [2:0] irq_pending;

    assign irq_pending = {
        (mem_count != 0),
        !csr_fifo_empty,
        !rd_fifo_empty
    };
    assign irq = |(reg_irq_en & irq_pending);






    //AXI4-Lite Write Channel
    logic [ADDR_WIDTH-1:0] aw_addr_reg;
    logic [DATA_WIDTH-1:0] w_data_reg;
    logic aw_done, w_done;

    //AW and W are handled independently
    assign s_axi_awready = !aw_done;
    assign s_axi_wready  = !w_done;

    always_ff @(posedge aclk or negedge aresetn) begin
        if (!aresetn) begin
            aw_done    <= 1'b0;
            w_done     <= 1'b0;
            aw_addr_reg <= '0;
            w_data_reg  <= '0;
        end else begin
            if (s_axi_awvalid && !aw_done) begin
                aw_done    <= 1'b1;
                aw_addr_reg <= s_axi_awaddr;
            end
            if (s_axi_wvalid && !w_done) begin
                w_done   <= 1'b1;
                w_data_reg <= s_axi_wdata;
            end
            //Clear when write response is accepted
            if (s_axi_bvalid && s_axi_bready) begin
                aw_done <= 1'b0;
                w_done  <= 1'b0;
            end
        end
    end

    logic wr_en;
    assign wr_en = aw_done && w_done && !s_axi_bvalid;

    //Write response
    always_ff @(posedge aclk or negedge aresetn) begin
        if (!aresetn)
            s_axi_bvalid <= 1'b0;
        else if (wr_en)
            s_axi_bvalid <= 1'b1;
        else if (s_axi_bready)
            s_axi_bvalid <= 1'b0;
    end
    assign s_axi_bresp = 2'b00;  // OKAY

    //Register writes
    logic [4:0] wr_addr;
    assign wr_addr = aw_addr_reg[6:2];

    always_ff @(posedge aclk or negedge aresetn) begin
        if (!aresetn) begin
            reg_instruction <= '0;
            reg_rs1_data    <= '0;
            reg_rs2_data    <= '0;
            reg_vtype       <= '0;
            reg_vl          <= '0;
            reg_vstart      <= '0;
            reg_vcsr        <= '0;
            reg_irq_en      <= '0;
        end else if (wr_en) begin
            case (wr_addr)
                5'h01: reg_instruction <= w_data_reg;       //0x04
                5'h02: reg_rs1_data    <= w_data_reg;       //0x08
                5'h03: reg_rs2_data    <= w_data_reg;       //0x0C
                5'h04: reg_vtype       <= w_data_reg;       //0x10
                5'h05: reg_vl          <= w_data_reg;       //0x14
                5'h06: reg_vstart      <= w_data_reg;       //0x18
                5'h07: reg_vcsr        <= w_data_reg;       //0x1C
                5'h0F: reg_irq_en      <= w_data_reg[2:0];  //0x3C
                default: ;
            endcase
        end
    end

    //CTRL write: issue_start (W1S at 0x00 bit[0])
    always_ff @(posedge aclk or negedge aresetn) begin
        if (!aresetn)
            issue_pending <= 1'b0;
        else if (issue_pending && issue_ready)
            issue_pending <= 1'b0;
        else if (wr_en && wr_addr == 5'h00 && w_data_reg[0])
            issue_pending <= 1'b1;
    end

    //MEM_COUNT W1C (write 1 to 0x38 bit[0] to decrement)
    assign mem_count_dec = wr_en && (wr_addr == 5'h0E) && w_data_reg[0];






    // AXI4-Lite Read Channel
    logic [4:0] rd_addr;
    assign rd_addr = s_axi_araddr[6:2];

    assign s_axi_arready = !s_axi_rvalid;

    always_ff @(posedge aclk or negedge aresetn) begin
        if (!aresetn) begin
            s_axi_rvalid <= 1'b0;
            s_axi_rdata  <= '0;
        end else if (s_axi_arvalid && s_axi_arready) begin
            s_axi_rvalid <= 1'b1;
            case (rd_addr)
                5'h00: s_axi_rdata <= {29'b0, issue_pending, issue_ready, 1'b0};  //CTRL
                5'h01: s_axi_rdata <= reg_instruction; //0x04
                5'h02: s_axi_rdata <= reg_rs1_data; //0x08
                5'h03: s_axi_rdata <= reg_rs2_data; //0x0C
                5'h04: s_axi_rdata <= reg_vtype; //0x10
                5'h05: s_axi_rdata <= reg_vl;//0x14
                5'h06: s_axi_rdata <= reg_vstart; //0x18
                5'h07: s_axi_rdata <= reg_vcsr; //0x1C
                5'h08: s_axi_rdata <= {27'b0, !rd_fifo_empty, rd_fifo_count[3:0]}; //0x20
                5'h09: s_axi_rdata <= rd_fifo_dout[31:0];  //0x24 RD_POP_DATA (pops FIFO)
                5'h0A: s_axi_rdata <= {26'b0, rd_meta_latched}; //0x28 RD_POP_META
                5'h0B: s_axi_rdata <= {27'b0, !csr_fifo_empty, csr_fifo_count[3:0]}; //0x2C
                5'h0C: s_axi_rdata <= csr_fifo_dout[31:0]; //0x30 CSR_POP (pops FIFO)
                5'h0D: s_axi_rdata <= csr_fflag_latched; //0x34
                5'h0E: s_axi_rdata <= {24'b0, mem_count}; //0x38
                5'h0F: s_axi_rdata <= {29'b0, reg_irq_en}; //0x3C
                5'h10: s_axi_rdata <= {29'b0, irq_pending}; //0x40
                default: s_axi_rdata <= '0;
            endcase
        end else if (s_axi_rready) begin
            s_axi_rvalid <= 1'b0;
        end
    end
    assign s_axi_rresp = 2'b00; //Okie done

    //FIFO pops triggered by read of the POP registers
    assign rd_fifo_pop  = s_axi_arvalid && s_axi_arready && (rd_addr == 5'h09);//0x24
    assign csr_fifo_pop = s_axi_arvalid && s_axi_arready && (rd_addr == 5'h0C);//0x30

endmodule
