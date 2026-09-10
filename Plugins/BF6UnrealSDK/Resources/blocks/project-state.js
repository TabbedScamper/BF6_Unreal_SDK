(function(root,factory){if(typeof module==='object'&&module.exports)module.exports=factory();else root.BF6ProjectState=factory();})(typeof globalThis!=='undefined'?globalThis:this,function(){
  'use strict';
  var copy=function(value){return JSON.parse(JSON.stringify(value));};
  function tops(doc){return doc&&doc.blocks&&doc.blocks.blocks||[];}
  function variables(base,live){
    var byId=new Map();
    (base.variables||[]).concat(live.variables||[]).forEach(function(v){byId.set(v.id,v);});
    base.variables=Array.from(byId.values());
  }
  function mergeRegion(document,canvas,ids,file){
    var result=copy(document),live=copy(canvas),active=new Set(ids),existing=new Map();
    tops(result).forEach(function(block){existing.set(block.id,block);});
    var incoming=tops(live),liveIds=new Set(incoming.map(function(b){return b.id;}));
    result.blocks=result.blocks||{};
    incoming.forEach(function(block){
      var old=existing.get(block.id);
      if(old){block.x=old.x;block.y=old.y;}
      var data={};try{data=JSON.parse(block.data||'{}');}catch(e){}
      if(block.type!=='modBlock'&&!data.file&&file&&file!==' rules'){data.file=file;block.data=JSON.stringify(data);}
    });
    var byId=new Map(incoming.map(function(b){return [b.id,b];}));
    result.blocks.blocks=tops(result).map(function(b){return byId.get(b.id)||(!active.has(b.id)?b:null);}).filter(Boolean);
    incoming.filter(function(b){return !existing.has(b.id);}).forEach(function(b){result.blocks.blocks.push(b);});
    variables(result,live);return result;
  }
  function mergeFocus(document,canvas,id){
    var result=copy(document),live=copy(canvas),replacement=tops(live).find(function(b){return b.id===id;}),found=false;
    function visit(value){
      if(!value||typeof value!=='object')return value;
      if(value.id===id&&value.type){
        found=true;
        if(!replacement)return value.next&&value.next.block||null;
        var block=copy(replacement);block.x=value.x;block.y=value.y;
        if(value.data!==undefined&&block.data===undefined)block.data=value.data;
        var tail=block;while(tail.next&&tail.next.block)tail=tail.next.block;
        if(value.next)tail.next=value.next;
        return block;
      }
      if(Array.isArray(value))return value.map(visit).filter(function(v){return v!==null;});
      Object.keys(value).forEach(function(key){var child=visit(value[key]);if(child===null&&key==='block')delete value[key];else value[key]=child;});return value;
    }
    result=visit(result);
    if(!found)throw Error('The focused block is missing from the saved project. Exit focused editing before saving.');
    result.blocks=result.blocks||{blocks:[]};
    tops(live).filter(function(b){return b.id!==id;}).forEach(function(b){result.blocks.blocks.push(b);});
    variables(result,live);return result;
  }
  return {mergeRegion:mergeRegion,mergeFocus:mergeFocus};
});
