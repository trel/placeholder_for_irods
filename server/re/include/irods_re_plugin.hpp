#ifndef IRODS_RE_PLUGIN_HPP
#define IRODS_RE_PLUGIN_HPP
#include "irods_error.hpp"
#include "irods_load_plugin.hpp"
#include "irods_lookup_table.hpp"
#include "irods_re_structs.hpp"

#include <boost/any.hpp>
#include <iostream>
#include <list>
#include <vector>
#include <utility>
#include <functional>
#include <map>
#include <memory>

namespace irods {

/* How to use
 * Get rei and set necessary fields
        ruleExecInfo_t rei;
        memset( ( char* )&rei, 0, sizeof( ruleExecInfo_t ) );
        rei.rsComm        = _comm;

 * Create rule_engine_context_manager
        rule_engine_context_manager<unit, ruleExecInfo_t*, AUDIT_RULE> re_ctx_mgr = rule_engine_context_manager<unit, ruleExecInfo_t*, AUDIT_RULE>(global_re_mgr, &rei);

 * to call any operation with dynamic pep
        dynamic_operation_execution_manager<T,C,DONT_AUDIT_RULE> rex_mgr (std::shared_ptr<rule_engine_context_manager<unit,ruleExecInfo_t*,DONT_AUDIT_RULE> > (new rule_engine_context_manager<unit,ruleExecInfo_t*,DONT_AUDIT_RULE >(global_re_mgr, &rei)));
        rex_mgr.call("instance", "op", op, args...);

 * If you want to determine if rule exists (not microservices)
        bool ret;
        error err;
        if ( !(err = re_ctx_mgr.rule_exists(_rn, ret)).ok() ) {
            return err;
        } else if( !ret ) {
            return ERROR( SYS_RULE_NOT_FOUND, "no rule found" );
        }

 * If you want to execute a rule, pass input parameter using rvalue or reference, output parameter using pointer
        std::string out;
        err = re_ctx_mgr.exec_rule(_rn, &out, args...);

*/
    template<typename T>
    T clone(T& a) {

        return a;
    }

    // the unpack constructor tell pluggable rule engine interface that the intention is to unpack the packed parameter list and call another rule
    // when the rule engine interface tries to pack the parameters again, it will instead pass them on to the plugin
    class unpack {
    public:
        unpack(std::list<boost::any> &_l);
        std::list<boost::any> &l_;
    };

    void var_arg_to_list(std::list<boost::any>& _l);

    template<typename... As>
    void var_arg_to_list(std::list<boost::any>& _l, unpack &&_p, As &&... _ps) {
        var_arg_to_list(_l, _p, std::forward<As>(_ps)...);
    }

    template<typename... As>
    void var_arg_to_list(std::list<boost::any>& _l, unpack &_p, As &&... _ps) {
        for (auto itr = begin (_p.l_) ; itr != end(_p.l_);++itr) {
            _l.push_back(boost::any(*itr));
        }
        var_arg_to_list(_l, std::forward<As>(_ps)...);

    }

    template<typename A, typename... As>
    void var_arg_to_list(std::list<boost::any>& _l,A&& _p, As&&... _ps) {
        _l.push_back(boost::any(_p));
        var_arg_to_list(_l, std::forward<As>(_ps)...);
    }

    error list_to_var_arg(std::list<boost::any>& _l);

    template<typename A, typename... As>
    error list_to_var_arg(std::list<boost::any>& _l, A&& _p, As&&... _ps) {
        if(_l.empty()) {
            return ERROR(RE_TYPE_ERROR, "arg list mismatch");
        } else {
            _p = boost::any_cast<A>(_l.front());
            _l.pop_front();
            return list_to_var_arg(_l, std::forward<As>(_ps)...);

        }
    }

    template<typename... As>
    std::list<boost::any> pack(As &&... _ps) {
        std::list<boost::any> l;
        var_arg_to_list(l, std::forward<As>(_ps)...);
        return l;
    }


    // microservice manager
    template<typename C>
    class default_microservice_manager {
    };

    template<typename C>
    using microservice_manager = default_microservice_manager<C>;

    enum rule_execution_manager_pack{
        AUDIT_RULE,
        DONT_AUDIT_RULE
    };

    template<typename T, typename C, rule_execution_manager_pack Audit>
    class rule_engine_context_manager;

    // callback
    // before we have concepts, we need to tightly couple this with rule_engine_context_manager to ensure typechecking
    class callback final {
    public:
        template<typename T, typename C, rule_execution_manager_pack Audit>
        callback(rule_engine_context_manager<T,C,Audit>& _callback_hdlr) : callback_hdlr_(&_callback_hdlr) {}

        template<typename... As>
        error operator()(std::string _rn, As&&... _ps);
    protected:
        boost::any callback_hdlr_;
    };

    // serialize wraps a function so that it can be called at runtime wihtout prior knowledge of original parameter types
    // only when the parameter type matches the oringal does the call succeed
    class serialize final {
    public:
        template<typename... As>
        serialize(std::function<error(As ...)> _op, std::string _op_name = "<unknown>") : op_(_op), op_name_(_op_name) { }

        template<typename... Bs>
        error operator()(Bs&&... _ps) {
            if(typeid(std::function<error(Bs...)>) == op_.type()) {
                return boost::any_cast<std::function<error(Bs...)> >(op_)(std::forward<Bs>(_ps)...);
            } else {
                return ERROR(RE_TYPE_ERROR, std::string("operation ") + op_name_ + " called with wrong types of arguments");
            }
        }
    protected:
        boost::any op_;
        std::string op_name_;
    };

    template <typename T>
    using re_plugin_operation = error(*) (T&, ...);


    template<typename T>
    class pluggable_rule_engine final {
    public:

        pluggable_rule_engine(const std::string &_in, const std::string &_context) : instance_name_(_in) {
        }

        template<typename... types_t>
        error add_operation(
                const std::string& _op,
                std::function<error(types_t...)> _f ) {
            if ( _op.empty() ) {
                std::stringstream msg;
                msg << "empty operation [" << _op << "]";
                return ERROR( SYS_INVALID_INPUT_PARAM, msg.str() );
            }
            operations_[_op] = _f;
            return SUCCESS();

        }

        error start_operation(T& _in) {
            auto fcn = boost::any_cast<std::function<error(T&,const std::string&)>>( operations_["start"] );
            return fcn(_in,instance_name_);
        }

        error stop_operation(T& _in) {
            auto fcn = boost::any_cast<std::function<error(T&,const std::string&)>>( operations_["stop"] );
            return fcn(_in,instance_name_);
        }

        error rule_exists(std::string _rn, T& _re_ctx, bool& _out) {
            auto fcn = boost::any_cast<std::function<error(T&,std::string,bool&)>>( operations_["rule_exists"] );
            return fcn(_re_ctx, _rn, _out);
        }

        template<typename ...As>
        error exec_rule(std::string _rn, T& _re_ctx, As&&... _ps, callback _callback) {
            auto l = pack(std::forward<As>(_ps)...);
            auto fcn = boost::any_cast<std::function<error(T&, std::string, std::list<boost::any> &, callback)>>( operations_["exec_rule"] );
            return fcn(_re_ctx, _rn, l, _callback);
        }

        template<typename ...As>
        error exec_rule_text(
                std::string _rt,
                T&          _re_ctx,
                callback    _callback,
                As&&...     _ps) {
            auto l = pack(std::forward<As>(_ps)...);
            auto fcn = boost::any_cast<
                           std::function<error(T&, std::string, std::list<boost::any> &, callback)>>(
                               operations_["exec_rule_text"] );
            return fcn(_re_ctx, _rt, l, _callback);
        }

        template<typename ...As>
        error exec_rule_expression(
                std::string _rt,
                T&          _re_ctx,
                callback    _callback,
                As&&...     _ps) {
            auto l = pack(std::forward<As>(_ps)...);
            auto fcn = boost::any_cast<
                           std::function<error(T&, std::string, std::list<boost::any> &, callback)>>(
                               operations_["exec_rule_expression"] );
            return fcn(_re_ctx, _rt, l, _callback);
        }

    private:
        error load_operation(void *handle, std::string _fcn, std::string _key);
        irods::lookup_table< boost::any > operations_;
        std::string instance_name_;
    };

    template<typename T, typename C, rule_execution_manager_pack Audit>
    class rule_engine_context_manager;

    template<typename T, typename C, rule_execution_manager_pack Audit>
    class dynamic_operation_execution_manager final {
    public:

        dynamic_operation_execution_manager(
            std::shared_ptr<rule_engine_context_manager<T,C,Audit> > _re_mgr  // rule engine manager
        ) : re_mgr_(_re_mgr) { }

        template<typename... As >
        error exec_rule_text(
            std::string _rule_text,
            std::string _instance_name,
            As&& ... _ps) {
                return re_mgr_->exec_rule_text(
                           _rule_text,
                           _instance_name,
                           clone(_ps)...);

        }

        template<typename... As >
        error exec_rule_expression(
            std::string _rule_text,
            std::string _instance_name,
            As&& ... _ps) {
                return re_mgr_->exec_rule_expression(
                           _rule_text,
                           _instance_name,
                           clone(_ps)...);

        }
        
        template<typename OP, typename... As >
        error call(
            std::string _instance_name,
            std::string _operation_name,
            OP _operation,
            As&& ... _ps) {
            std::string rule_name = "pep";
            // =-=-=-=-=-=-=-
            // debug message for creating dynPEP rules
            rodsLog(
                LOG_DEBUG,
                "exec_op [%s]",
                _operation_name.c_str() );
            // general pep rule
            // pep(<inst name>, <op name>, <go op>, <go with different params op>, <params>, <callback>)
            // rule must call op explicitly if it is defined
            // inputs are passed as some lists of some non-pointers
            // outputs are passed as some lists of some pointers
            bool execOnce = false;
            std::function<error()> op = [&execOnce, &_ps..., &_operation] () {
                if(execOnce) {
                    return ERROR(RE_TYPE_ERROR, "cannot execute operation more than once with client supplied arguments");
                } else {
                    execOnce = true;
                    return _operation(std::forward<As>(_ps)...);
                }
            };
            bool ret;
            error err;
            if(!(err = re_mgr_->rule_exists(rule_name, ret)).ok() || !ret) { // run op only if rule is not defined
                err = op();
            } else {
                err = re_mgr_->exec_rule(rule_name, _instance_name, _operation_name, op, serialize(_operation, _operation_name), clone(_ps)...);
            }
            return err;
        }
    protected:

        std::shared_ptr<rule_engine_context_manager<T,C,Audit> >re_mgr_;

    }; // class dynamic_operation_execution_manager


    // DONE
    template<typename T>
    struct re_pack_inp final {
        std::string instance_name_;
        std::string plugin_name_;
        T re_ctx_;
        pluggable_rule_engine<T> *re_;
        re_pack_inp(std::string _instance_name, std::string _plugin_name, T _re_ctx) : instance_name_(_instance_name), plugin_name_(_plugin_name), re_ctx_(_re_ctx) { }
    };

    // load rule engines from plugins DONE
    template<typename T>
    class rule_engine_plugin_manager final {
    public:
        double interface_version() { return 1.0; }
        rule_engine_plugin_manager(std::string _dir) : dir_(_dir) { }

        ~rule_engine_plugin_manager() {
            for (auto itr = begin(re_plugin_map_);itr != end(re_plugin_map_); ++itr) delete itr->second;
        }

        error resolve(std::string& _plugin_name, const std::string& _inst_name, pluggable_rule_engine<T> *& _re_ptr) {
            auto itr = re_plugin_map_.find(_inst_name);
            if(itr == end(re_plugin_map_)) {
                error err = load_plugin <pluggable_rule_engine<T> > (_re_ptr, _plugin_name, dir_, _inst_name, std::string("empty_context"));
                if (!err.ok()) {
                    irods::log( PASS( err ) );
                    return err;
                }
                re_plugin_map_[_inst_name] = _re_ptr;
            } else {
                _re_ptr = itr->second;
            }
            return SUCCESS();
        }

        std::map<std::string, pluggable_rule_engine<T> *> re_plugin_map_;

    protected:
        std::string dir_;

    };


    // load rule engines from plugins DONE
    template<typename T, typename C>
    class rule_engine_manager final {
    public:
        rule_engine_manager(rule_engine_plugin_manager <T>& _re_plugin_mgr, std::vector<re_pack_inp<T> > &_re_packs, microservice_manager<C> &_ms_mgr) : ms_mgr_(_ms_mgr), re_plugin_mgr_(_re_plugin_mgr) {
            std::for_each(begin(_re_packs), end(_re_packs), [this](re_pack_inp<T> &_inp) {
                error err = this->init_rule_engine(_inp);
                if( !err.ok() ) {
                    irods::log( PASS( err ) );
                }
            });
        }

        ~rule_engine_manager() {
            std::for_each(begin(re_packs_), end(re_packs_), [](re_pack_inp<T> &_inp) {
                _inp.re_->stop_operation(_inp.re_ctx_);
            });
        }

        error init_rule_engine(re_pack_inp<T>& _inp) {
            pluggable_rule_engine<T>* pre = NULL;
            error err;

            err = re_plugin_mgr_.resolve(_inp.plugin_name_, _inp.instance_name_, pre);
            if(!err.ok()) {
                return PASS( err );
            }

            pre->start_operation(_inp.re_ctx_);
            if(!err.ok()) {
                return PASS( err );
            }

            _inp.re_ = pre;
            re_packs_.push_back(_inp);

            return SUCCESS();
        }

        microservice_manager<C> &ms_mgr_;
        std::list<re_pack_inp<T> > re_packs_;
    protected:
        rule_engine_plugin_manager<T> &re_plugin_mgr_;

    };




    template <typename ER, typename EM, typename T, typename ...As>
    inline error control(std::list<re_pack_inp<T> >& _re_packs, ER _er, EM _em, std::string _rn, As &&... _ps) {
        bool ret;
        for(auto itr = begin(_re_packs); itr != end(_re_packs); ++itr) {
            if (itr->re_->rule_exists(_rn, itr->re_ctx_, ret).ok() && ret) {
                return _er(*itr, _rn, std::forward<As>(_ps)...);
            }
        }
        return _em(_rn, std::forward<As>(_ps)...);
    }



    template<typename T, typename C>
    class rule_exists_manager {
    public:
        rule_exists_manager(rule_engine_manager<T,C>& _re_mgr) :
                re_mgr_(_re_mgr) {}

        virtual ~rule_exists_manager() {}

        error rule_exists(std::string _rn, bool& ret) {
            auto er = [&ret] (re_pack_inp<T>&, std::string) {
                ret = true;
                return SUCCESS();
            };

            auto em = [&ret] (std::string) {
                ret = false;
                return SUCCESS();
            };
            return control(re_mgr_.re_packs_, er, em, _rn);
        }
    protected:
        rule_engine_manager<T,C> &re_mgr_;
    };


    // rule_engine_manager + ctx DONE
    template<typename T, typename C, rule_execution_manager_pack Audit>
    class rule_engine_context_manager {};

    template<typename T, typename C>
    class rule_engine_context_manager<T,C,AUDIT_RULE> final : public rule_exists_manager<T,C> {
    public:
        rule_engine_context_manager(rule_engine_manager<T,C>& _re_mgr, C _ctx) :
                rule_exists_manager<T,C>(_re_mgr),
                ctx_(_ctx),
                rex_mgr_( std::shared_ptr<rule_engine_context_manager<T,C,DONT_AUDIT_RULE> >(new rule_engine_context_manager<T,C,DONT_AUDIT_RULE >(_re_mgr, _ctx))) {}


        template <typename ...As>
        error exec_rule(std::string _rn, As &&... _ps) {
            auto er = [this](re_pack_inp<T>& _itr, std::string _rn, decltype(_ps)... _ps) {
                std::function<error(std::string, re_pack_inp<T>&, decltype(_ps)...)> fun =
                [this](std::string _rn, re_pack_inp<T>& _itr, decltype(_ps)... _ps) {
                    return _itr.re_->template exec_rule<As...>(_rn, this->ctx_, std::forward<As >(_ps)..., callback(*this));
                };
                return this->rex_mgr_.call(_itr.instance_name_,std::string ("exec_rule"), fun, _rn, _itr, std::forward<As>(_ps)...);
            };
            auto em = [this](std::string _rn, decltype(_ps) ... _ps) {
                std::function<error(std::string, C&, decltype(_ps)...)> fun =
                [this](std::string _rn, C& _ctx, decltype(_ps) ... _ps) {
                    return this->re_mgr_.ms_mgr_.exec_microservice(_rn, _ctx, std::forward<As>(_ps)...);
                };
                return this->rex_mgr_.call(std::string ("microservice_manager"),std::string ("exec_microservice"), fun, _rn, this->ctx_, std::forward<As>(_ps)...);
            };
            return control(this->re_mgr_.re_packs_, er, em, _rn, std::forward<As>(_ps)...);
        }

        template <typename ...As>
        error exec_rule_text(std::string _rt, std::string instance_name_, As &&... _ps) {
            error ret;
            for( auto itr  = begin(this->re_mgr_.re_packs_);
                      itr != end(this->re_mgr_.re_packs_);
                            ++itr ) {
                if( instance_name_ == itr->instance_name_ ) {
                    ret = this->rex_mgr_.exec_rule_text(
                            _rt,
                            itr->instance_name_,
                            itr->re_ctx_,
                            callback(*this),
                            std::forward<As>(_ps)...);
                    return ret;
                }
            }

            std::string msg( "instance not found [" );
            msg += instance_name_;
            msg += "]";
            return ERROR(
                       SYS_INVALID_INPUT_PARAM,
                       msg );
        }

        template <typename ...As>
        error exec_rule_expression(std::string _rt, std::string instance_name_, As &&... _ps) {
            error ret;
            for( auto itr  = begin(this->re_mgr_.re_packs_);
                      itr != end(this->re_mgr_.re_packs_);
                            ++itr ) {
                if( instance_name_ == itr->instance_name_ ) {
                    ret = this->rex_mgr_.exec_rule_expression(
                            _rt,
                            itr->instance_name_,
                            itr->re_ctx_,
                            callback(*this),
                            std::forward<As>(_ps)...);
                    return ret;
                }
            }

            std::string msg( "instance not found [" );
            msg += instance_name_;
            msg += "]";
            return ERROR(
                       SYS_INVALID_INPUT_PARAM,
                       msg );
        }

    protected:
        C ctx_;
        dynamic_operation_execution_manager<T,C,DONT_AUDIT_RULE> rex_mgr_;

    };

    template<typename T, typename C>
    class rule_engine_context_manager<T,C,DONT_AUDIT_RULE> final : public rule_exists_manager<T,C> {
    public:

        rule_engine_context_manager(rule_engine_manager<T,C>& _re_mgr, C _ctx) :
                rule_exists_manager<T,C>(_re_mgr),
                ctx_(_ctx) {}

        template <typename ...As>
        error exec_rule(std::string _rn, As &&... _ps) {
            auto er = [this](re_pack_inp<T>& _itr, std::string _rn, decltype(_ps)... _ps) {
                return _itr.re_->template exec_rule<As...>( _rn, this->ctx_, std::forward<As>(_ps)..., callback(*this));
            };
            auto em = [this](std::string _rn, decltype(_ps)... _ps) {
                return this->re_mgr_.ms_mgr_.exec_microservice(_rn, this->ctx_, std::forward<As>(_ps)...);
            };
            return control(this->re_mgr_.re_packs_, er, em, _rn, std::forward<As>(_ps)...);
        }

        template <typename ...As>
        error exec_rule_text(std::string _rt, std::string instance_name_, As &&... _ps) {
            error ret;
            for( auto itr  = begin(this->re_mgr_.re_packs_);
                      itr != end(this->re_mgr_.re_packs_);
                            ++itr ) {
                if( instance_name_ == itr->instance_name_ ) {
                    ret = itr->re_->exec_rule_text(
                            _rt,
                            itr->re_ctx_,
                            callback(*this),
                            std::forward<As>(_ps)...);
                    return ret;
                }
            }

            std::string msg( "instance not found [" );
            msg += instance_name_;
            msg += "]";
            return ERROR(
                       SYS_INVALID_INPUT_PARAM,
                       msg );
        }

        template <typename ...As>
        error exec_rule_expression(std::string _rt, std::string instance_name_, As &&... _ps) {
            error ret;
            for( auto itr  = begin(this->re_mgr_.re_packs_);
                      itr != end(this->re_mgr_.re_packs_);
                            ++itr ) {
                if( instance_name_ == itr->instance_name_ ) {
                    ret = itr->re_->exec_rule_expression(
                            _rt,
                            itr->re_ctx_,
                            callback(*this),
                            std::forward<As>(_ps)...);
                    return ret;
                }
            }
 
            std::string msg( "instance not found [" );
            msg += instance_name_;
            msg += "]";
            return ERROR(
                       SYS_INVALID_INPUT_PARAM,
                       msg );
        }

    protected:
        C ctx_;

    };

    template<typename T>
    error pluggable_rule_engine<T>::load_operation(void *_handle, std::string _fcn, std::string _key) {
        dlerror();
        re_plugin_operation<T> plugin_op_ptr = reinterpret_cast< re_plugin_operation<T> > (dlsym(_handle, _fcn.c_str()));
        if (!plugin_op_ptr) {
            return ERROR(-1, "cannot load plugin operation " + _key + " " + _fcn);
        }
        operations_[_key] = plugin_op_ptr;
	return SUCCESS();
    }

    using unit = ruleExecInfo_t *;
#define UNIT NULL

    using default_re_ctx = ruleExecInfo_t *;
    using default_ms_ctx = ruleExecInfo_t *;

    template<>
    class default_microservice_manager<default_ms_ctx> {
    public:
        error exec_microservice_adapter(std::string _msn, default_ms_ctx _ctx, std::list<boost::any>&);

        template<typename... As>
        error exec_microservice(std::string _msn, default_ms_ctx _ctx, As&&... _ps) {
            // =-=-=-=-=-=-=-
            // debug message for creating dynPEP rules
            rodsLog(
                LOG_DEBUG,
                "microservice_manager::exec_microservice_no_audit [%s]",
                _msn.c_str() );

            // =-=-=-=-=-=-=-
            // add the output parameter

            auto l = pack(std::forward<As>(_ps)...);
            error err = exec_microservice_adapter(_msn, _ctx, l);

            return err;

        }

    };

    template<typename... As>
    error callback::operator()(std::string _rn,  As&& ... _ps) {
        if(callback_hdlr_.type() == typeid(rule_engine_context_manager<default_re_ctx, default_ms_ctx, AUDIT_RULE> *)) {
            auto cb = boost::any_cast<rule_engine_context_manager<default_re_ctx, default_ms_ctx, AUDIT_RULE> *>(callback_hdlr_);
            return cb->exec_rule(_rn, std::forward<As> (_ps)...);
        } else if (callback_hdlr_.type() == typeid(rule_engine_context_manager<default_re_ctx, default_ms_ctx, DONT_AUDIT_RULE> *)) {
            auto cb = boost::any_cast<rule_engine_context_manager<default_re_ctx, default_ms_ctx, DONT_AUDIT_RULE> *>(callback_hdlr_);
            return cb->exec_rule(_rn, std::forward<As> (_ps)...);
        } else {
            rodsLog(
                LOG_ERROR,
                "rule engine callback: unsupported effect handler type. encountered:\n%s\nexpected:\n%s\n%s\n",
                callback_hdlr_.type().name(),
                typeid(rule_engine_context_manager<default_re_ctx, default_ms_ctx, AUDIT_RULE> *).name(),
                typeid(rule_engine_context_manager<default_re_ctx, default_ms_ctx, DONT_AUDIT_RULE> *).name()
            );
            return ERROR(RE_TYPE_ERROR, "unsupported effect handler type");
        }
    }

    error convertToMsParam(boost::any &itr, msParam_t *t);
    error convertFromMsParam(boost::any& itr, msParam_t *t);

    template <typename T>
    T any_extract(boost::any _p, std::function<T()> f) {
        if(_p.type() == typeid(T*)) {
            return *boost::any_cast<T*>(_p);

        } else if(_p.type() == typeid(T)){
            return boost::any_cast<T>(_p);

        } else {
            // rodsLog(LOG_ERROR, (std::string("cannot convert type ") + _p.type().name() + " to " + typeid(T).name()).c_str());
            return f();
        }
    }

    template <typename T>
    void any_update(boost::any& _p, T _p2) {
        if(_p.type() == typeid(T*)) {
            *boost::any_cast<T*>(_p) = _p2;
        }
    }

    template<typename S, typename T, typename F>
    void foreach2(S&& a, T&& b, F&& f) {
        typename std::remove_reference<S>::type::iterator itr = a.begin();
        typename std::remove_reference<T>::type::iterator itr2 = b.begin();
        for(;itr!=a.end() && itr2!=b.end();++itr, ++itr2) {
          f(*itr, *itr2);
        }
    }

    template< typename InputC, typename OutputC, typename UnaryOperation >
    void transform(const InputC &ic, OutputC &oc, UnaryOperation func) {
        std::transform(ic.begin(), ic.end(), oc.begin(), func);
    }

    template< typename InputC, typename OutputC, typename UnaryOperation >
    OutputC transform(const InputC &ic,  UnaryOperation func) {
        OutputC oc;
        std::transform(ic.begin(), ic.end(), oc.begin(), func);
        return oc;
    }

    std::vector<re_pack_inp<default_re_ctx> > init_global_re_packs();
    struct global_re_plugin_mgr {
        microservice_manager<default_ms_ctx> global_ms_mgr;
        std::vector<re_pack_inp<default_re_ctx> > global_re_packs = init_global_re_packs();
        rule_engine_plugin_manager<default_re_ctx> global_re_plugin_mgr = rule_engine_plugin_manager<default_re_ctx>(PLUGIN_TYPE_RULE_ENGINE);
        rule_engine_manager<default_re_ctx, default_ms_ctx> global_re_mgr = rule_engine_manager<default_re_ctx, default_ms_ctx>(global_re_plugin_mgr, global_re_packs, global_ms_mgr);
    };

    extern std::unique_ptr<struct global_re_plugin_mgr> re_plugin_globals;

}
#define DEFINE_FACTORY \
    irods::pluggable_rule_engine<irods::default_re_ctx>* plugin_factory(const std::string& _inst_name, const std::string& _context) { \
        return new irods::pluggable_rule_engine<irods::default_re_ctx>( _inst_name , _context); \
    } \
    double get_plugin_interface_version() { \
        static const double PLUGIN_INTERFACE_VERSION = 1.0; \
        return PLUGIN_INTERFACE_VERSION; \
    }

#endif
