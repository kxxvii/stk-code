//  $Id$
//
//  SuperTuxKart - a fun racing game with go-kart
//  Copyright (C) 2006 Joerg Henrichs
//
//  This program is free software; you can redistribute it and/or
//  modify it under the terms of the GNU General Public License
//  as published by the Free Software Foundation; either version 3
//  of the License, or (at your option) any later version.
//
//  This program is distributed in the hope that it will be useful,
//  but WITHOUT ANY WARRANTY; without even the implied warranty of
//  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
//  GNU General Public License for more details.
//
//  You should have received a copy of the GNU General Public License
//  along with this program; if not, write to the Free Software
//  Foundation, Inc., 59 Temple Place - Suite 330, Boston, MA  02111-1307, USA.

#ifndef HEADER_ITEMMANAGER_HPP
#define HEADER_ITEMMANAGER_HPP


#include <vector>
#include <map>
#include "items/item.hpp"
#include "lisp/lisp.hpp"

class Kart;

class ItemManager
{

private:
    /** The vector of all items of the current track. */
    typedef std::vector<Item*> AllItemTypes;
    AllItemTypes m_all_items;

    /** This stores all item models. */
    scene::IMesh *m_item_mesh[Item::ITEM_LAST-Item::ITEM_FIRST+1];

    /** Stores all meshes for all items. */
    std::map<std::string,scene::IMesh*> m_all_meshes;

    std::string m_user_filename;
    void setDefaultItemStyle();
    void setItem(const lisp::Lisp *item_node, const char *colour,
                 Item::ItemType type);
    void           loadItemStyle   (const std::string filename);

public:
                   ItemManager();
                  ~ItemManager();
    void           loadDefaultItems();
    Item*          newItem         (Item::ItemType type, const Vec3& xyz, 
                                    const Vec3 &normal, Kart* parent=NULL);
    void           update          (float delta);
    void           hitItem         (Kart* kart);
    void           cleanup         ();
    void           reset           ();
    void           removeTextures  ();
    void           setUserFilename (char *s) {m_user_filename=s;}
    void           collectedItem   (int item_id, Kart *kart,
                                    int add_info=-1);
    void           setStyle        ();
    scene::IMesh*  getItemModel    (Item::ItemType type)
                                      {return m_item_mesh[type];}
    scene::IMesh*  getOtherModel   (const std::string modelName)
                                      {assert(m_all_meshes.count(modelName) == 1);
                                       return m_all_meshes[modelName];}
};

extern ItemManager* item_manager;

#endif
