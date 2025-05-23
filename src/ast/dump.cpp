/*
 * MRustC - Mutabah's Rust Compiler
 * - By John Hodge (Mutabah/thePowersGang)
 *
 * ast/dump.cpp
 * - Dumps the AST of a crate as rust code (annotated)
 */
#include <ast/crate.hpp>
#include <ast/ast.hpp>
#include <ast/expr.hpp>
#include <main_bindings.hpp>
#include <hir/hir.hpp>  // ABI_RUST - TODO: Move elsewhere?
#include <fstream>
#include <limits> // std::numeric_limits

#include <cpp_unpack.h>

#define IS(v, c)    (dynamic_cast<c*>(&v) != 0)
#define WRAPIF_CMD(v, t)  || IS(v, t)
#define WRAPIF(uniq_ptr, class1, ...) do { auto& _v = *(uniq_ptr); if( IS(_v, class1) CC_ITERATE(WRAPIF_CMD, (_v), __VA_ARGS__) ) { paren_wrap(uniq_ptr); } else { AST::NodeVisitor::visit(uniq_ptr); } } while(0)

class RustPrinter:
    public AST::NodeVisitor
{
    ::std::ostream& m_os;
    int m_indent_level;
    bool m_expr_root;   //!< used to allow 'if' and 'match' to behave differently as standalone exprs
public:
    RustPrinter(::std::ostream& os):
        m_os(os),
        m_indent_level(0),
        m_expr_root(false)
    {}

    void handle_module(const AST::Module& mod);
    void handle_struct(const AST::Struct& s);
    void handle_enum(const AST::Enum& s);
    void handle_trait(const AST::Trait& s);

    void handle_function(const AST::Visibility& vis, const RcString& name, const AST::Function& f);

    virtual bool is_const() const override { return true; }
    virtual void visit(AST::ExprNode_Block& n) override {
        switch(n.m_block_type) {
        case AST::ExprNode_Block::Type::Bare:
            break;
        case AST::ExprNode_Block::Type::Unsafe:
            m_os << "unsafe ";
            break;
        case AST::ExprNode_Block::Type::Const:
            m_os << "const ";
            break;
        }
        m_os << "{";
        inc_indent();
        if( n.m_local_mod )
        {
            m_os << "\n";
            m_os << indent() << "// ANON: " << n.m_local_mod->path() << "\n";
            handle_module(*n.m_local_mod);
        }
        bool is_first = true;
        for( auto& child : n.m_nodes )
        {
            if(is_first) {
                is_first = false;
            } else {
                m_os << ";";
            }
            m_os << "\n";
            if( child ) {
                this->print_attrs(child->attrs());
            }
            m_os << indent();
            m_expr_root = true;
            if( !child.get() )
                m_os << "/* nil */";
            else
                AST::NodeVisitor::visit(child);
        }
        if( !n.m_yields_final_value )
            m_os << ";";
        m_os << "\n";
        dec_indent();
        m_os << indent() << "}";
    }
    virtual void visit(AST::ExprNode_Try& n) override {
        m_os << "try ";
        AST::NodeVisitor::visit(n.m_inner);
    }
    void dump_token(const Token& t) {
        m_os << t.to_str() << " ";
    }
    void dump_tokentree(const TokenTree& tt) {
        if( tt.is_token() ) {
            dump_token(tt.tok());
        }
        else {
            for(size_t i = 0; i < tt.size(); i ++)
            {
                dump_tokentree(tt[i]);
            }
        }
    }
    virtual void visit(AST::ExprNode_Macro& n) override {
        m_expr_root = false;
        m_os << n.m_path << "!";
        if( n.m_ident != "" ) {
            m_os << " ";
            m_os << n.m_ident;
        }
        m_os << (n.m_is_braced ? "{" : "(");
        dump_tokentree(n.m_tokens);
        m_os << (n.m_is_braced ? "}" : ")");
    }
    virtual void visit(AST::ExprNode_Asm& n) override {
        m_os << "asm!( \"" << n.m_text << "\"";
        m_os << " :";
        for(auto& v : n.m_output)
        {
            m_os << " \"" << v.name << "\" (";
            AST::NodeVisitor::visit(v.value);
            m_os << "),";
        }
        m_os << " :";
        for(auto& v : n.m_input)
        {
            m_os << " \"" << v.name << "\" (";
            AST::NodeVisitor::visit(v.value);
            m_os << "),";
        }
        m_os << " :";
        for(const auto& v : n.m_clobbers)
            m_os << " \"" << v << "\",";
        m_os << " :";
        for(const auto& v : n.m_flags)
            m_os << " \"" << v << "\",";
        m_os << " )";
    }
    virtual void visit(AST::ExprNode_Asm2& n) override {
        m_os << "asm!( ";
        for(const auto& l : n.m_lines)
        {
            l.fmt(m_os);
            m_os << ", ";
        }
        for(auto& p : n.m_params)
        {
            TU_MATCH_HDRA((p), {)
            TU_ARMA(Const, e) {
                m_os << "const ";
                AST::NodeVisitor::visit(e);
                }
            TU_ARMA(Sym, e) {
                m_os << "sym " << e;
                }
            TU_ARMA(RegSingle, e) {
                m_os << e.dir << "(" << e.spec << ") ";
                AST::NodeVisitor::visit(e.val);
                }
            TU_ARMA(Reg, e) {
                m_os << e.dir << "(" << e.spec << ") ";
                if(e.val_in) {
                    AST::NodeVisitor::visit(e.val_in);
                    if(e.val_out)
                        m_os << " => ";
                }
                if(e.val_out)
                    AST::NodeVisitor::visit(e.val_out);
                }
            }
            m_os << ", ";
        }
        if(n.m_options.any())
        {
            n.m_options.fmt(m_os);
            //m_os << "options(";
            //m_os << ")";
        }
        m_os << ")";
    }
    virtual void visit(AST::ExprNode_Flow& n) override {
        m_expr_root = false;
        switch(n.m_type)
        {
        case AST::ExprNode_Flow::RETURN:    m_os << "return ";  break;
        case AST::ExprNode_Flow::YIELD:     m_os << "yield ";  break;
        case AST::ExprNode_Flow::BREAK:     m_os << "break ";  break;
        case AST::ExprNode_Flow::CONTINUE:  m_os << "continue ";  break;
        case AST::ExprNode_Flow::YEET:      m_os << "do yeet ";  break;
        }
        AST::NodeVisitor::visit(n.m_value);
    }
    virtual void visit(AST::ExprNode_LetBinding& n) override {
        m_expr_root = false;
        m_os << "let ";
        print_pattern(n.m_pat, false);
        m_os << ": ";
        print_type(n.m_type);
        if( n.m_value ) {
            m_os << " = ";
            AST::NodeVisitor::visit(n.m_value);
        }
        m_os << ";";
    }
    virtual void visit(AST::ExprNode_Assign& n) override {
        m_expr_root = false;
        AST::NodeVisitor::visit(n.m_slot);
        switch(n.m_op)
        {
        case AST::ExprNode_Assign::NONE:    m_os << "  = ";  break;
        case AST::ExprNode_Assign::ADD:     m_os << " += ";  break;
        case AST::ExprNode_Assign::SUB:     m_os << " -= ";  break;
        case AST::ExprNode_Assign::MUL:     m_os << " *= ";  break;
        case AST::ExprNode_Assign::DIV:     m_os << " /= ";  break;
        case AST::ExprNode_Assign::MOD:     m_os << " %= ";  break;
        case AST::ExprNode_Assign::AND:     m_os << " &= ";  break;
        case AST::ExprNode_Assign::OR:      m_os << " |= ";  break;
        case AST::ExprNode_Assign::XOR:     m_os << " ^= ";  break;
        case AST::ExprNode_Assign::SHR:     m_os << " >>= ";  break;
        case AST::ExprNode_Assign::SHL:     m_os << " <<= ";  break;
        }
        AST::NodeVisitor::visit(n.m_value);
    }
    virtual void visit(AST::ExprNode_CallPath& n) override {
        m_expr_root = false;
        m_os << n.m_path;
        m_os << "(";
        bool is_first = true;
        for( auto& arg : n.m_args )
        {
            if(is_first) {
                is_first = false;
            } else {
                m_os << ", ";
            }
            AST::NodeVisitor::visit(arg);
        }
        m_os << ")";
    }
    virtual void visit(AST::ExprNode_CallMethod& n) override {
        m_expr_root = false;
        WRAPIF( n.m_val
            , AST::ExprNode_Deref, AST::ExprNode_UniOp
            , AST::ExprNode_Cast, AST::ExprNode_BinOp, AST::ExprNode_Assign
            , AST::ExprNode_Match, AST::ExprNode_If, AST::ExprNode_IfLet, AST::ExprNode_Match
            );
        m_os << "." << n.m_method;
        m_os << "(";
        bool is_first = true;
        for( auto& arg : n.m_args )
        {
            if(is_first) {
                is_first = false;
            } else {
                m_os << ", ";
            }
            AST::NodeVisitor::visit(arg);
        }
        m_os << ")";
    }
    virtual void visit(AST::ExprNode_CallObject& n) override {
        m_expr_root = false;
        m_os << "(";
        AST::NodeVisitor::visit(n.m_val);
        m_os << ")(";
        bool is_first = true;
        for( auto& arg : n.m_args )
        {
            if(is_first) {
                is_first = false;
            } else {
                m_os << ", ";
            }
            AST::NodeVisitor::visit(arg);
        }
        m_os << ")";
    }
    virtual void visit(AST::ExprNode_Loop& n) override {
        bool expr_root = m_expr_root;
        m_expr_root = false;

        switch(n.m_type)
        {
        case AST::ExprNode_Loop::LOOP:
            m_os << "loop";
            break;
        case AST::ExprNode_Loop::WHILE:
            m_os << "while ";
            AST::NodeVisitor::visit(n.m_cond);
            break;
        case AST::ExprNode_Loop::FOR:
            m_os << "for ";
            print_pattern(n.m_pattern, true);
            m_os << " in ";
            AST::NodeVisitor::visit(n.m_cond);
            break;
        }

        if( expr_root )
        {
            m_os << "\n";
            m_os << indent();
        }
        else
        {
            m_os << " ";
        }

        AST::NodeVisitor::visit(n.m_code);
    }
    void visit_iflet_conditions(std::vector<AST::IfLet_Condition>& conds) {
        for(size_t i = 0; i < conds.size(); i ++) {
            if(i != 0) m_os << " && ";
            if(conds[i].opt_pat) {
                if( i > 0 ) m_os << "let ";
                print_pattern(*conds[i].opt_pat, true);
                m_os << " = ";
            }
            m_os << "(";
            AST::NodeVisitor::visit(conds[i].value);
            m_os << ")";
        }
    }
    void visit(AST::ExprNode_WhileLet& n) override {
        bool expr_root = m_expr_root;
        m_expr_root = false;

        m_os << "while let ";
        visit_iflet_conditions(n.m_conditions);
        if( expr_root )
        {
            m_os << "\n";
            m_os << indent();
        }
        else
        {
            m_os << " ";
        }

        AST::NodeVisitor::visit(n.m_code);
    }
    virtual void visit(AST::ExprNode_Match& n) override {
        bool expr_root = m_expr_root;
        m_expr_root = false;
        m_os << "match ";
        AST::NodeVisitor::visit(n.m_val);

        if(expr_root)
        {
            m_os << "\n";
            m_os << indent() << "{\n";
        }
        else
        {
            m_os << " {\n";
            inc_indent();
        }

        for( auto& arm : n.m_arms )
        {
            m_os << indent();
            bool is_first = true;
            for( const auto& pat : arm.m_patterns ) {
                if(!is_first)
                    m_os << "|";
                is_first = false;
                print_pattern(pat, true);
            }
            if( !arm.m_guard.empty() ) {
                m_os << " if ";
                visit_iflet_conditions(arm.m_guard);
            }
            m_os << " => ";
            // Increase indent, but don't print. Causes nested blocks to be indented above the match
            inc_indent();
            AST::NodeVisitor::visit(arm.m_code);
            dec_indent();
            m_os << ",\n";
        }

        if(expr_root)
        {
            m_os << indent() << "}";
        }
        else
        {
            m_os << indent() << "}";
            dec_indent();
        }
    }
    virtual void visit(AST::ExprNode_If& n) override {
        bool expr_root = m_expr_root;
        m_expr_root = false;
        m_os << "if ";
        AST::NodeVisitor::visit(n.m_cond);

        visit_if_common(expr_root, n.m_true, n.m_false);
    }
    virtual void visit(AST::ExprNode_IfLet& n) override {
        bool expr_root = m_expr_root;
        m_expr_root = false;
        m_os << "if let ";
        visit_iflet_conditions(n.m_conditions);

        visit_if_common(expr_root, n.m_true, n.m_false);
    }
    void visit_if_common(bool expr_root, ::AST::ExprNodeP& tv, ::AST::ExprNodeP& fv)
    {
        if( expr_root )
        {
            m_os << "\n";
            m_os << indent();
        }
        else
        {
            m_os << " ";
        }

        bool is_block = (dynamic_cast<const AST::ExprNode_Block*>(&*tv) != nullptr);
        if( !is_block ) m_os << "{ ";
        AST::NodeVisitor::visit(tv);
        if( !is_block ) m_os << " }";
        if(fv.get())
        {
            if( expr_root )
            {
                m_os << "\n";
                m_os << indent() << "else";
                // handle chained if statements nicely
                if( IS(*fv, AST::ExprNode_If) || IS(*fv, AST::ExprNode_IfLet) ) {
                    m_expr_root = true;
                    m_os << " ";
                }
                else
                    m_os << "\n" << indent();
            }
            else
            {
                m_os << " else ";
            }
            AST::NodeVisitor::visit(fv);
        }
    }
    virtual void visit(AST::ExprNode_Closure& n) override {
        m_expr_root = false;
        m_os << "|";
        bool is_first = true;
        for( const auto& arg : n.m_args )
        {
            if(!is_first)   m_os << ", ";
            is_first = false;
            print_pattern(arg.first, false);
            m_os << ": ";
            print_type(arg.second);
        }
        m_os << "| ->";
        print_type(n.m_return);
        m_os << " { ";
        AST::NodeVisitor::visit(n.m_code);
        m_os << " }";
    }
    virtual void visit(AST::ExprNode_WildcardPattern& n) override {
        m_os << "_";
    }
    virtual void visit(AST::ExprNode_Integer& n) override {
        m_expr_root = false;
        switch(n.m_datatype)
        {
        case CORETYPE_INVAL:
            m_os << "0x" << ::std::hex << n.m_value << ::std::dec << "_/*INVAL*/";
            break;
        case CORETYPE_BOOL:
        case CORETYPE_STR:
            m_os << "0x" << ::std::hex << n.m_value << ::std::dec << "_/*bool/str*/";
            break;
        case CORETYPE_CHAR:
            //if( 0x20 <= n.m_value && n.m_value < 128 ) {
            if( n.m_value >= 0x20 && n.m_value < 128 ) {
                switch(n.m_value.truncate_u64())
                {
                case '\'':  m_os << "'\\''";    break;
                case '\\':  m_os << "'\\\\'";   break;
                default:
                    m_os << "'" << (char)n.m_value.truncate_u64() << "'";
                    break;
                }
            }
            else {
                m_os << "'\\u{" << ::std::hex << n.m_value << ::std::dec << "}'";
            }
            break;
        case CORETYPE_F32:
        case CORETYPE_F64:
            break;
        case CORETYPE_U8:
        case CORETYPE_U16:
        case CORETYPE_U32:
        case CORETYPE_U64:
        case CORETYPE_U128:
        case CORETYPE_UINT:
        case CORETYPE_ANY:
            m_os << "0x" << ::std::hex << n.m_value << ::std::dec;
            break;
        case CORETYPE_I8:
        case CORETYPE_I16:
        case CORETYPE_I32:
        case CORETYPE_I64:
        case CORETYPE_I128:
        case CORETYPE_INT:
            m_os << n.m_value;
            break;
        }
    }
    virtual void visit(AST::ExprNode_Float& n) override {
        m_expr_root = false;
        switch(n.m_datatype)
        {
        case CORETYPE_F32:
            m_os.precision(::std::numeric_limits<float>::max_digits10 + 1);
            m_os << n.m_value;
            break;
        case CORETYPE_ANY:
        case CORETYPE_F64:
            m_os.precision(::std::numeric_limits<double>::max_digits10 + 1);
            m_os << n.m_value;
            break;
        default:
            break;
        }
    }
    virtual void visit(AST::ExprNode_Bool& n) override {
        m_expr_root = false;
        if( n.m_value )
            m_os << "true";
        else
            m_os << "false";
    }
    virtual void visit(AST::ExprNode_String& n) override {
        m_expr_root = false;
        m_os << "\"" << FmtEscaped(n.m_value) << "\"";
    }
    virtual void visit(AST::ExprNode_ByteString& n) override {
        m_expr_root = false;
        m_os << "b\"" << FmtEscaped(n.m_value) << "\"";
    }

    virtual void visit(AST::ExprNode_StructLiteral& n) override {
        m_expr_root = false;
        m_os << n.m_path << " {\n";
        inc_indent();
        for( auto& i : n.m_values )
        {
            // TODO: Attributes
            m_os << indent() << i.name << ": ";
            AST::NodeVisitor::visit(i.value);
            m_os << ",\n";
        }
        if( n.m_base_value.get() )
        {
            m_os << indent() << ".. ";
            AST::NodeVisitor::visit(n.m_base_value);
            m_os << "\n";
        }
        m_os << indent() << "}";
        dec_indent();
    }
    virtual void visit(AST::ExprNode_StructLiteralPattern& n) override {
        m_expr_root = false;
        m_os << n.m_path << " {\n";
        inc_indent();
        for( auto& i : n.m_values )
        {
            // TODO: Attributes
            m_os << indent() << i.name << ": ";
            AST::NodeVisitor::visit(i.value);
            m_os << ",\n";
        }
        m_os << indent() << "..\n";
        m_os << indent() << "}";
        dec_indent();
    }
    virtual void visit(AST::ExprNode_Array& n) override {
        m_expr_root = false;
        m_os << "[";
        if( n.m_size.get() )
        {
            AST::NodeVisitor::visit(n.m_values[0]);
            m_os << "; ";
            AST::NodeVisitor::visit(n.m_size);
        }
        else {
            for( auto& item : n.m_values )
            {
                AST::NodeVisitor::visit(item);
                m_os << ", ";
            }
        }
        m_os << "]";
    }
    virtual void visit(AST::ExprNode_Tuple& n) override {
        m_expr_root = false;
        m_os << "(";
        for( auto& item : n.m_values )
        {
            AST::NodeVisitor::visit(item);
            m_os << ", ";
        }
        m_os << ")";
    }
    virtual void visit(AST::ExprNode_NamedValue& n) override {
        m_expr_root = false;
        m_os << n.m_path;
    }
    virtual void visit(AST::ExprNode_Field& n) override {
        m_expr_root = false;
        WRAPIF( n.m_obj
            , AST::ExprNode_Deref, AST::ExprNode_UniOp
            , AST::ExprNode_Cast, AST::ExprNode_BinOp, AST::ExprNode_Assign
            , AST::ExprNode_Match, AST::ExprNode_If, AST::ExprNode_IfLet, AST::ExprNode_Match
            );
        m_os << "." << n.m_name;
    }
    virtual void visit(AST::ExprNode_Index& n) override {
        m_expr_root = false;
        WRAPIF( n.m_obj
            , AST::ExprNode_Deref, AST::ExprNode_UniOp
            , AST::ExprNode_Cast, AST::ExprNode_BinOp, AST::ExprNode_Assign
            , AST::ExprNode_Match, AST::ExprNode_If, AST::ExprNode_IfLet, AST::ExprNode_Match
            );
        m_os << "[";
        AST::NodeVisitor::visit(n.m_idx);
        m_os << "]";
    }
    virtual void visit(AST::ExprNode_Deref& n) override {
        m_expr_root = false;
        m_os << "*(";
        AST::NodeVisitor::visit(n.m_value);
        m_os << ")";
    }
    virtual void visit(AST::ExprNode_Cast& n) override {
        m_expr_root = false;
        m_os << "(";
        AST::NodeVisitor::visit(n.m_value);
        m_os << ") as " << n.m_type;
    }
    virtual void visit(AST::ExprNode_TypeAnnotation& n) override {
        m_expr_root = false;
        m_os << "(";
        AST::NodeVisitor::visit(n.m_value);
        m_os << ") : " << n.m_type;
    }
    virtual void visit(AST::ExprNode_BinOp& n) override {
        m_expr_root = false;
        if( !n.m_left ) {
            m_os << "/*null*/";
        }
        else if( IS(*n.m_left, AST::ExprNode_BinOp) && dynamic_cast<AST::ExprNode_BinOp&>(*n.m_left).m_type == n.m_type ) {
            AST::NodeVisitor::visit(n.m_left);
        }
        else {
            WRAPIF(n.m_left
                , AST::ExprNode_Cast, AST::ExprNode_BinOp
                );
        }
        m_os << " ";
        switch(n.m_type)
        {
        case AST::ExprNode_BinOp::CMPEQU: m_os << "=="; break;
        case AST::ExprNode_BinOp::CMPNEQU:m_os << "!="; break;
        case AST::ExprNode_BinOp::CMPLT:  m_os << "<";  break;
        case AST::ExprNode_BinOp::CMPLTE: m_os << "<="; break;
        case AST::ExprNode_BinOp::CMPGT:  m_os << ">";  break;
        case AST::ExprNode_BinOp::CMPGTE: m_os << ">="; break;
        case AST::ExprNode_BinOp::BOOLAND:m_os << "&&"; break;
        case AST::ExprNode_BinOp::BOOLOR: m_os << "||"; break;
        case AST::ExprNode_BinOp::BITAND: m_os << "&";  break;
        case AST::ExprNode_BinOp::BITOR:  m_os << "|";  break;
        case AST::ExprNode_BinOp::BITXOR: m_os << "^";  break;
        case AST::ExprNode_BinOp::SHL:    m_os << "<<"; break;
        case AST::ExprNode_BinOp::SHR:    m_os << ">>"; break;
        case AST::ExprNode_BinOp::MULTIPLY: m_os << "*"; break;
        case AST::ExprNode_BinOp::DIVIDE:   m_os << "/"; break;
        case AST::ExprNode_BinOp::MODULO:   m_os << "%"; break;
        case AST::ExprNode_BinOp::ADD:   m_os << "+"; break;
        case AST::ExprNode_BinOp::SUB:   m_os << "-"; break;
        case AST::ExprNode_BinOp::RANGE: m_os << ".."; break;
        case AST::ExprNode_BinOp::RANGE_INC: m_os << "..."; break;
        case AST::ExprNode_BinOp::PLACE_IN: m_os << "<-"; break;
        }
        m_os << " ";
        if( !n.m_right ) {
            m_os << "/*null*/";
        }
        else if( IS(*n.m_right, AST::ExprNode_BinOp) && dynamic_cast<AST::ExprNode_BinOp&>(*n.m_right).m_type != n.m_type ) {
            paren_wrap(n.m_right);
        }
        else
            AST::NodeVisitor::visit(n.m_right);
    }
    virtual void visit(AST::ExprNode_UniOp& n) override {
        m_expr_root = false;
        switch(n.m_type)
        {
        case AST::ExprNode_UniOp::NEGATE:   m_os << "-";    break;
        case AST::ExprNode_UniOp::INVERT:   m_os << "!";    break;
        case AST::ExprNode_UniOp::BOX:      m_os << "box ";    break;
        case AST::ExprNode_UniOp::REF:    m_os << "&";    break;
        case AST::ExprNode_UniOp::REFMUT: m_os << "&mut ";    break;
        case AST::ExprNode_UniOp::RawBorrow:    m_os << "&raw const "; break;
        case AST::ExprNode_UniOp::RawBorrowMut: m_os << "&raw mut "; break;
        case AST::ExprNode_UniOp::QMARK: break;
        case AST::ExprNode_UniOp::AWait: break;
        }

        if( IS(*n.m_value, AST::ExprNode_BinOp) )
            m_os << "(";
        AST::NodeVisitor::visit(n.m_value);
        if( IS(*n.m_value, AST::ExprNode_BinOp) )
            m_os << ")";
        switch(n.m_type)
        {
        case AST::ExprNode_UniOp::QMARK: m_os << "?"; break;
        case AST::ExprNode_UniOp::AWait: m_os << ".await";  break;
        default:    break;
        }
    }


private:
    void paren_wrap(::AST::ExprNodeP& node) {
        m_os << "(";
        AST::NodeVisitor::visit(node);
        m_os << ")";
    }

    void print_attrs(const AST::AttributeList& attrs);
    void print_params(const AST::GenericParams& params);
    void print_bounds(const AST::GenericParams& params);
    void print_pattern_tuple(const AST::Pattern::TuplePat& v, bool is_refutable);
    void print_pattern(const AST::Pattern& p, bool is_refutable);
    void print_type(const TypeRef& t);

    void inc_indent();
    RepeatLitStr indent();
    void dec_indent();
};

void RustPrinter::print_attrs(const AST::AttributeList& attrs)
{
    for(const auto& a : attrs.m_items)
    {
        m_os << indent() << "#[" << a << "]\n";
    }
}

void RustPrinter::handle_module(const AST::Module& mod)
{
    bool need_nl = true;

    for( const auto& ip : mod.m_items )
    {
        const auto& i = *ip;
        if( !i.data.is_Use() )  continue ;
        const auto& i_data = i.data.as_Use();
        //if(need_nl) {
        //    m_os << "\n";
        //    need_nl = false;
        //}
        if( i_data.entries.empty() ) {
            continue ;
        }
        m_os << indent() << i.vis << "use ";
        if( i_data.entries.size() > 1 ) {
            m_os << "{";
        }
        for(const auto& ent : i_data.entries)
        {
            if( &ent != &i_data.entries.front() )
                m_os << ", ";
            m_os << ent.path;
            if( ent.name == "" ) {
                m_os << "::*";
            }
            else if( ent.path.nodes().size() > 0 && ent.name != ent.path.nodes().back().name() ) {
                m_os << " as " << ent.name;
            }
            else {
            }
        }
        if( i_data.entries.size() > 1 ) {
            m_os << "}";
        }
        m_os << ";\n";
    }
    need_nl = true;

    for( const auto& ip : mod.m_items )
    {
        const auto& item = *ip;
        if( !item.data.is_Crate() )    continue ;
        const auto& e = item.data.as_Crate();

        print_attrs(item.attrs);
        m_os << indent() << "extern crate \"" << e.name << "\" as " << item.name << ";\n";
    }

    for( const auto& ip : mod.m_items )
    {
        const auto& item = *ip;
        if( !item.data.is_ExternBlock() )    continue ;
        const auto& e = item.data.as_ExternBlock();

        print_attrs(item.attrs);
        m_os << indent() << "extern \"" << e.abi() << "\" {}\n";
    }

    for( const auto& ip : mod.m_items )
    {
        const auto& item = *ip;
        if( !item.data.is_Module() )    continue ;
        const auto& e = item.data.as_Module();

        m_os << "\n";
        m_os << indent() << item.vis << "mod " << item.name << "\n";
        m_os << indent() << "{\n";
        inc_indent();
        handle_module(e);
        dec_indent();
        m_os << indent() << "}\n";
        m_os << "\n";
    }

    for( const auto& ip : mod.m_items )
    {
        const auto& item = *ip;
        if( !item.data.is_Type() )    continue ;
        const auto& e = item.data.as_Type();

        if(need_nl) {
            m_os << "\n";
            need_nl = false;
        }
        print_attrs(item.attrs);
        m_os << indent() << item.vis << "type " << item.name;
        print_params(e.params());
        m_os << " = " << e.type();
        print_bounds(e.params());
        m_os << ";\n";
    }
    need_nl = true;

    for( const auto& ip : mod.m_items )
    {
        const auto& item = *ip;
        if( !item.data.is_Struct() )    continue ;
        const auto& e = item.data.as_Struct();

        m_os << "\n";
        print_attrs(item.attrs);
        m_os << indent() << item.vis << "struct " << item.name;
        handle_struct(e);
    }

    for( const auto& ip : mod.m_items )
    {
        const auto& item = *ip;
        if( !item.data.is_Enum() )    continue ;
        const auto& e = item.data.as_Enum();

        m_os << "\n";
        print_attrs(item.attrs);
        m_os << indent() << item.vis << "enum " << item.name;
        handle_enum(e);
    }

    for( const auto& ip : mod.m_items )
    {
        const auto& item = *ip;
        if( !item.data.is_Trait() )    continue ;
        const auto& e = item.data.as_Trait();

        m_os << "\n";
        print_attrs(item.attrs);
        m_os << indent() << item.vis << "trait " << item.name;
        handle_trait(e);
    }

    for( const auto& ip : mod.m_items )
    {
        const auto& item = *ip;
        if( !item.data.is_Static() )    continue ;
        const auto& e = item.data.as_Static();

        if(need_nl) {
            m_os << "\n";
            need_nl = false;
        }
        print_attrs(item.attrs);
        m_os << indent() << item.vis;
        switch( e.s_class() )
        {
        case AST::Static::CONST:  m_os << "const ";   break;
        case AST::Static::STATIC: m_os << "static ";   break;
        case AST::Static::MUT:    m_os << "static mut ";   break;
        }
        m_os << item.name << ": " << e.type() << " = ";
        e.value().visit_nodes(*this);
        m_os << ";\n";
    }

    for( const auto& ip : mod.m_items )
    {
        const auto& item = *ip;
        if( !item.data.is_Function() )    continue ;
        const auto& e = item.data.as_Function();

        m_os << "\n";
        print_attrs(item.attrs);
        handle_function(item.vis, item.name, e);
    }

    for( const auto& ip : mod.m_items )
    {
        const auto& item = *ip;
        if( !item.data.is_Impl() )    continue ;
        const auto& i = item.data.as_Impl();

        m_os << "\n";
        m_os << indent() << "impl";
        print_params(i.def().params());
        if( i.def().trait().ent != AST::Path() )
        {
                m_os << " " << i.def().trait().ent << " for";
        }
        m_os << " " << i.def().type() << "\n";

        print_bounds(i.def().params());
        m_os << indent() << "{\n";
        inc_indent();
        for( const auto& it : i.items() )
        {
            TU_MATCH_DEF(AST::Item, (*it.data), (e),
            (
                throw ::std::runtime_error(FMT("Unexpected item type in impl block - " << it.data->tag_str()));
                ),
            (None,
                // Ignore, it's been deleted by #[cfg]
                ),
            (MacroInv,
                // TODO: Dump macro invocations
                ),
            (Static,
                m_os << indent();
                switch(e.s_class())
                {
                case ::AST::Static::CONST:  m_os << "const ";   break;
                case ::AST::Static::STATIC: m_os << "static ";  break;
                case ::AST::Static::MUT:    m_os << "static mut ";  break;
                }
                m_os << it.name << ": " << e.type() << " = ";
                e.value().visit_nodes(*this);
                m_os << ";\n";
                ),
            (Type,
                m_os << indent() << "type " << it.name << " = " << e.type() << ";\n";
                ),
            (Function,
                handle_function(it.vis, it.name, e);
                )
            )
        }
        dec_indent();
        m_os << indent() << "}\n";
    }

    // HACK: Assume that anon modules have been printed already, so don't include them here.
    // - Needed, because this code is used for proc macro output, which doen't like the `#<n>` syntax
    #if 0
    for(const auto& m : mod.anon_mods())
    {
        if(!m) {
            m_os << indent() << "/* mod ? (delted anon) */\n";
            continue ;
        }
        m_os << indent() << "mod " << m->path().nodes.back() << " {\n";
        inc_indent();
        handle_module(*m);
        dec_indent();
        m_os << indent() << "}\n";
    }
    #endif
}

void RustPrinter::print_params(const AST::GenericParams& params)
{
    if( !params.m_params.empty() )
    {
        bool is_first = true;
        m_os << "<";
        for( const auto& p : params.m_params )
        {
            if( !is_first )
                m_os << ", ";
            TU_MATCH_HDRA( (p), {)
            TU_ARMA(None, p) {
                m_os << "/*-*/";
                }
            TU_ARMA(Lifetime, p) {
                m_os << p;
                }
            TU_ARMA(Type, p) {
                m_os << p.attrs();
                m_os << p.name();
                if( !p.get_default().is_wildcard() )
                    m_os << " = " << p.get_default();
                }
            TU_ARMA(Value, p) {
                m_os << p.attrs();
                m_os << "const " << p.name() << ": " << p.type();
                }
            }
            is_first = false;
        }
        m_os << ">";
    }
}

void RustPrinter::print_bounds(const AST::GenericParams& params)
{
    if( !params.m_bounds.empty() )
    {
        inc_indent();
        bool is_first = true;

        for( const auto& b : params.m_bounds )
        {
            if( b.is_None() ) {
                m_os << "/*-*/";
                continue ;
            }
            if( !is_first ) {
                m_os << ",\n";
            }
            else {
                m_os << indent() << "where\n";
            }
            is_first = false;

            m_os << indent();
            TU_MATCH(AST::GenericBound, (b), (ent),
            (None,
                m_os << "/*-*/";
                ),
            (Lifetime,
                m_os << ent.test << ": " << ent.bound;
                ),
            (TypeLifetime,
                m_os << ent.type << ": " << ent.bound;
                ),
            (IsTrait,
                m_os << ent.outer_hrbs << ent.type << ": " << ent.inner_hrbs << ent.trait;
                ),
            (MaybeTrait,
                m_os << ent.type << ": ?" << ent.trait;
                ),
            (NotTrait,
                m_os << ent.type << ": !" << ent.trait;
                ),
            (Equality,
                m_os << ent.type << ": =" << ent.replacement;
                )
            )
        }
        m_os << "\n";

        dec_indent();
    }
}

void RustPrinter::print_pattern_tuple(const AST::Pattern::TuplePat& v, bool is_refutable)
{
    for(const auto& sp : v.start) {
        print_pattern(sp, is_refutable);
        m_os << ", ";
    }
    if( v.has_wildcard )
    {
        m_os << ".., ";
        for(const auto& sp : v.end) {
            print_pattern(sp, is_refutable);
            m_os << ", ";
        }
    }
}
void RustPrinter::print_pattern(const AST::Pattern& p, bool is_refutable)
{
    for( const auto& pb : p.bindings() ) {
        if( pb.m_mutable )
            m_os << "mut ";
        switch(pb.m_type)
        {
        case ::AST::PatternBinding::Type::MOVE:
            break;
        case ::AST::PatternBinding::Type::REF:
            m_os << "ref ";
            break;
        case ::AST::PatternBinding::Type::MUTREF:
            m_os << "ref mut ";
            break;
        }
        m_os << pb.m_name << "/*" << pb.m_slot << "*/";
        // If binding is irrefutable, and would be binding against a wildcard, just emit the name
        if( !is_refutable && p.bindings().size() == 1 && p.data().is_Any() )
        {
            return ;
        }
        m_os << " @ ";
    }
    TU_MATCH(AST::Pattern::Data, (p.data()), (v),
    (Any,
        m_os << "_";
        ),
    (MaybeBind,
        m_os << v.name << " /*?*/";
        ),
    (Macro,
        m_os << *v.inv;
        ),
    (Box, {
        const auto& v = p.data().as_Box();
        m_os << "& ";
        print_pattern(*v.sub, is_refutable);
        }),
    (Ref, {
        const auto& v = p.data().as_Ref();
        if(v.mut)
            m_os << "&mut ";
        else
            m_os << "& ";
        print_pattern(*v.sub, is_refutable);
        }),
    (Value,
        m_os << v.start;
        if( ! v.end.is_Invalid() ) {
            m_os << " ..= " << v.end;
        }
        ),
    (ValueLeftInc,
        m_os << v.start << " .. " << v.end;
        ),
    (StructTuple,
        m_os << v.path << "(";
        this->print_pattern_tuple(v.tup_pat, is_refutable);
        m_os << ")";
        ),
    (Struct, {
        const auto& v = p.data().as_Struct();
        m_os << v.path << "{";
        for(const auto& sp : v.sub_patterns) {
            m_os << sp.name << ": ";
            print_pattern(sp.pat, is_refutable);
            m_os << ",";
        }
        if( v.is_exhaustive ) {
            m_os << "..";
        }
        m_os << "}";
        }),
    (Tuple,
        m_os << "(";
        this->print_pattern_tuple(v, is_refutable);
        m_os << ")";
        ),
    (Slice,
        m_os << "[";
        for(const auto& sp : v.sub_pats) {
            print_pattern(sp, is_refutable);
            m_os << ", ";
        }
        m_os << "]";
        ),
    (SplitSlice,
        m_os << "[";
        bool needs_comma = false;
        for(const auto& sp : v.leading) {
            print_pattern(sp, is_refutable);
            m_os << ", ";
        }

        if(v.extra_bind.is_valid())
        {
            const auto& b = v.extra_bind;
            if( b.m_mutable )
                m_os << "mut ";
            switch(b.m_type)
            {
            case ::AST::PatternBinding::Type::MOVE:
                break;
            case ::AST::PatternBinding::Type::REF:
                m_os << "ref ";
                break;
            case ::AST::PatternBinding::Type::MUTREF:
                m_os << "ref mut ";
                break;
            }
            m_os << b.m_name << "/*"<<b.m_slot<<"*/";
        }
        m_os << "..";
        needs_comma = true;

        if(v.trailing.size()) {
            if( needs_comma ) {
                m_os << ", ";
            }
            for(const auto& sp : v.trailing) {
                print_pattern(sp, is_refutable);
                m_os << ", ";
            }
        }
        m_os << "]";
        ),
    (Or,
        m_os << "(";
        for(const auto& e : v) {
            m_os << (&e == &v.front() ? "" : " | ");
            print_pattern(e, is_refutable);
        }
        m_os << ")";
        )
    )
}

void RustPrinter::print_type(const TypeRef& t)
{
    m_os << t;
}

void RustPrinter::handle_struct(const AST::Struct& s)
{
    print_params(s.params());

    TU_MATCH(AST::StructData, (s.m_data), (e),
    (Unit,
        m_os << " /* unit-like */\n";
        print_bounds(s.params());
        m_os << indent() << ";\n";
        ),
    (Tuple,
        m_os << "(";
        for( const auto& i : e.ents ) {
            m_os << i.m_vis << i.m_type << ", ";
        }
        m_os << ")\n";
        print_bounds(s.params());
        m_os << indent() << ";\n";
        ),
    (Struct,
        m_os << "\n";
        print_bounds(s.params());

        m_os << indent() << "{\n";
        inc_indent();
        for( const auto& i : e.ents )
        {
            m_os << indent() << i.m_vis << i.m_name << ": " << i.m_type.print_pretty() << ",\n";
        }
        dec_indent();
        m_os << indent() << "}\n";
        )
    )
    m_os << "\n";
}

void RustPrinter::handle_enum(const AST::Enum& s)
{
    print_params(s.params());
    m_os << "\n";
    print_bounds(s.params());

    m_os << indent() << "{\n";
    inc_indent();
    unsigned int idx = 0;
    for( const auto& i : s.variants() )
    {
        m_os << indent() << "/*"<<idx<<"*/" << i.m_name;
        TU_MATCH(AST::EnumVariantData, (i.m_data), (e),
        (Value,
            m_os << " = " << e.m_value;
            ),
        (Tuple,
            m_os << "(";
            for( const auto& t : e.m_items )
                m_os << t.m_type.print_pretty() << ", ";
            m_os << ")";
            ),
        (Struct,
            m_os << "{\n";
            inc_indent();
            for( const auto& i : e.m_fields )
            {
                m_os << indent() << i.m_name << ": " << i.m_type.print_pretty() << "\n";
            }
            dec_indent();
            m_os << indent() << "}";
            )
        )
        m_os << ",\n";
        idx ++;
    }
    dec_indent();
    m_os << indent() << "}\n";
    m_os << "\n";
}

void RustPrinter::handle_trait(const AST::Trait& s)
{
    print_params(s.params());
    {
        char c = ':';
        for(const auto& lft : s.lifetimes()) {
            m_os << " " << c << " " << lft.ent;
            c = '+';
        }
        for(const auto& t : s.supertraits()) {
            m_os << " " << c << " " << t.ent.hrbs << *t.ent.path;
            c = '+';
        }
    }
    m_os << "\n";
    print_bounds(s.params());

    m_os << indent() << "{\n";
    inc_indent();

    for( const auto& i : s.items() )
    {
        TU_MATCH_DEF(AST::Item, (i.data), (e),
        (
            ),
        (Type,
            m_os << indent() << "type " << i.name << ";\n";
            ),
        (Function,
            handle_function(AST::Visibility::make_bare_private(), i.name, e);
            )
        )
    }

    dec_indent();
    m_os << indent() << "}\n";
    m_os << "\n";
}

void RustPrinter::handle_function(const AST::Visibility& vis, const RcString& name, const AST::Function& f)
{
    m_os << indent();
    m_os << vis;
    if( f.is_const() )
        m_os << "const ";
    if( f.is_unsafe() )
        m_os << "unsafe ";
    if( f.is_async() )
        m_os << "async ";
    if( f.abi() != ABI_RUST )
        m_os << "extern \"" << f.abi() << "\" ";
    m_os << "fn " << name;
    print_params(f.params());
    m_os << "(";
    bool is_first = true;
    for( const auto& a : f.args() )
    {
        if( !is_first )
            m_os << ", ";
        print_attrs(a.attrs);
        print_pattern( a.pat, false );
        m_os << ": " << a.ty.print_pretty();
        is_first = false;
    }
    m_os << ")";
    if( !f.rettype().is_unit() )
    {
        m_os << " -> " << f.rettype().print_pretty();
    }

    if( f.code().is_valid() )
    {
        m_os << "\n";
        print_bounds(f.params());

        m_os << indent();
        f.code().visit_nodes(*this);
        m_os << "\n";
        //m_os << indent() << f.data.code() << "\n";
    }
    else
    {
        print_bounds(f.params());
        m_os << ";\n";
    }
}

void RustPrinter::inc_indent()
{
    m_indent_level ++;
}
RepeatLitStr RustPrinter::indent()
{
    return RepeatLitStr { "    ", m_indent_level };
}
void RustPrinter::dec_indent()
{
    m_indent_level --;
}

void Dump_Rust(const char *filename, const AST::Crate& crate)
{
    ::std::ofstream os(filename);
    RustPrinter printer(os);
    printer.handle_module(crate.root_module());
}
void DumpAST_Node(::std::ostream& os, const AST::ExprNode& node)
{
    RustPrinter printer(os);
    const_cast<AST::ExprNode&>( node ).visit(printer);
}
