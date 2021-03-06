-- |Computations in the vectorisation monad concerned with naming and fresh variable generation.

module Vectorise.Monad.Naming
  ( mkLocalisedName
  , mkVectId
  , cloneVar
  , newExportedVar
  , newLocalVar
  , newLocalVars
  , newDummyVar
  , newTyVar
  ) where

import Vectorise.Monad.Base

import DsMonad
import Type
import Var
import Name
import SrcLoc
import Id
import FastString

import Control.Monad


-- Naming ---------------------------------------------------------------------

-- |Create a localised variant of a name, using the provided function to transform its `OccName`.
--
-- If the name external, encode the orignal name's module into the new 'OccName'.  The result is
-- always an internal system name.
--
mkLocalisedName :: (Maybe String -> OccName -> OccName) -> Name -> VM Name
mkLocalisedName mk_occ name = 
  do { mod <- liftDs getModuleDs
     ; u   <- liftDs newUnique
     ; let occ_name = mkLocalisedOccName mod mk_occ name

           new_name | isExternalName name = mkExternalName u mod occ_name (nameSrcSpan name)
                    | otherwise           = mkSystemName   u     occ_name

     ; return new_name
     }

-- |Produce the vectorised variant of an `Id` with the given type.
--
-- Force the new name to be a system name and, if the original was an external name, disambiguate
-- the new name with the module name of the original.
--
mkVectId :: Id -> Type -> VM Id
mkVectId id ty
  = do { name <- mkLocalisedName mkVectOcc (getName id)
       ; let id' | isExportedId id = Id.mkExportedLocalId name ty
                 | otherwise       = Id.mkLocalId         name ty
       ; return id'
       }

-- |Make a fresh instance of this var, with a new unique.
--
cloneVar :: Var -> VM Var
cloneVar var = liftM (setIdUnique var) (liftDs newUnique)

-- |Make a fresh exported variable with the given type.
--
newExportedVar :: OccName -> Type -> VM Var
newExportedVar occ_name ty 
 = do mod <- liftDs getModuleDs
      u   <- liftDs newUnique

      let name = mkExternalName u mod occ_name noSrcSpan
      
      return $ Id.mkExportedLocalId name ty

-- |Make a fresh local variable with the given type.
-- The variable's name is formed using the given string as the prefix.
--
newLocalVar :: FastString -> Type -> VM Var
newLocalVar fs ty
 = do u <- liftDs newUnique
      return $ mkSysLocal fs u ty

-- |Make several fresh local variables with the given types.
-- The variable's names are formed using the given string as the prefix.
--
newLocalVars :: FastString -> [Type] -> VM [Var]
newLocalVars fs = mapM (newLocalVar fs)

-- |Make a new local dummy variable.
--
newDummyVar :: Type -> VM Var
newDummyVar = newLocalVar (fsLit "vv")

-- |Make a fresh type variable with the given kind.
-- The variable's name is formed using the given string as the prefix.
--
newTyVar :: FastString -> Kind -> VM Var
newTyVar fs k
 = do u <- liftDs newUnique
      return $ mkTyVar (mkSysTvName u fs) k
